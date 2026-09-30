/* RACCOON V2 - simulator link server.
 *
 * Serves the real tuner protocol over TCP, in front of the real ECU, the real flash layer
 * and the real calibration validator. That combination is the point: it lets the whole
 * tuner - connect, unlock, read, edit, stage, verify, activate - be exercised with no
 * hardware attached, against code that is the same code that will run on the bench.
 *
 * It is deliberately not a mock. A Python reimplementation of the ECU would be a second
 * ECU, free to agree with whatever the client happens to expect, and every serious defect
 * this project has found so far lived in the gap between the two: a scheduler that never
 * fired an event, a fuel model working in barometric units, a validator that validated
 * nothing, a live frame carrying zeros where it declared sensor readings. A mock cannot
 * find any of those, because a mock agrees.
 */
#include "rc_sim_link.h"

#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include "rc/cal.h"
#include "rc/rc_crc32.h"
#include "rc/rc_link.h"
#include "rc/rc_link_service.h"

#define LINK_MEDIUM_SIZE 0x30000u
#define LINK_SLOT0_BASE 0x10000u
#define LINK_SLOT1_BASE 0x20000u
#define LINK_MARKER_BASE 0x28000u

/** Consecutive sends that could not proceed. Reset by every successful one; when it
 *  outlasts patience, the client is declared gone rather than allowed to stall the ECU. */
static unsigned stalled;

/* A plain memory medium for the flash layer, the same shape the unit tests use. A real SPI
 * part would prove more about the driver, and less about the protocol this server exists
 * to exercise. */
/* ctx is `void *` because that is the signature the HAL declares; making it const would
 * require casting the function pointer itself, which buys nothing and hides the real
 * interface. The region is only read, and the local below says so. */
// cppcheck-suppress constParameterCallback
static int link_medium_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    const uint8_t *region = (const uint8_t *)ctx;
    if ((size_t)offset + (size_t)len > LINK_MEDIUM_SIZE) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(dst, &region[offset], len);
    return RC_HAL_OK;
}

static int link_medium_write(void *ctx, uint32_t offset, const void *src, uint32_t len)
{
    uint8_t *region = (uint8_t *)ctx;
    if ((size_t)offset + (size_t)len > LINK_MEDIUM_SIZE) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(&region[offset], src, len);
    return RC_HAL_OK;
}

static const rc_engine_state_t *link_live(void *ctx)
{
    return &((rc_harness_t *)ctx)->control.state;
}

static bool link_armed(void *ctx)
{
    return ((rc_harness_t *)ctx)->armed;
}

static uint32_t link_faults(void *ctx)
{
    /* The ECU's own sensor-health bitmask, not the simulator's injected-fault switches: a
     * tuner's diagnostics page is about what the ECU believes about its own sensors, and
     * showing the test rig's configuration would answer a question nobody asked. */
    return ((rc_harness_t *)ctx)->control.sensors.health_mask;
}

void rc_sim_link_init(rc_sim_link_t *link, rc_harness_t *harness, uint16_t log_hz,
                      const char *unlock_code)
{
    memset(link, 0, sizeof(*link));
    /* Every descriptor starts at -1, explicitly and all of them. The memset leaves them at
     * 0, which is stdin: a guard written as `if (client >= 0)` then passes, and the server
     * spends its life reading a console instead of a socket - with no error, and with the
     * symptom that the control port accepts connections the server never sees. */
    link->listen_fd = -1;
    link->client_fd = -1;
    link->control_fd = -1;
    link->control_client = -1;
    link->now_us = 0u;
    link->now_ms = 0u;

    /* 0xFF, not 0x00: an erased flash cell reads as 0xFF, and a medium that starts at zero
     * would make "this slot has never been written" and "this slot contains a valid but
     * empty calibration" look the same to the layers above. */
    memset(link->medium, 0xFF, sizeof(link->medium));
    /* The HAL lives in the struct, not in a local. rc_flash_init and rc_link_service_init
     * both keep the pointer they are handed, so a HAL on this function's stack is a
     * dangling pointer the moment the function returns - and the first thing that touches
     * it is a flash read, which then fails or corrupts something, a long way from here. */
    memset(&link->hal, 0, sizeof(link->hal));
    link->hal.storage_read = link_medium_read;
    link->hal.storage_write = link_medium_write;
    (void)rc_flash_init(&link->flash, &link->hal, link->medium, LINK_SLOT0_BASE,
                        LINK_SLOT1_BASE, LINK_MARKER_BASE);

    rc_link_service_init(&link->service, &link->hal, link->medium, &link->flash);
    rc_cal_t loaded;
    if (rc_flash_load_active(&link->flash, &loaded) == 0) {
        rc_link_service_adopt(&link->service, &loaded);
    } else {
        /* No slot is valid yet - a blank ECU. Start the tuner off the shipped defaults so
         * there is something coherent to read, and let the first stage write the real
         * thing. This mirrors what the firmware does on a first boot. */
        rc_link_service_adopt(&link->service, NULL);
    }
    link->service.live_source = link_live;
    link->service.live_ctx = harness;
    link->service.armed_source = link_armed;
    link->service.armed_ctx = harness;
    link->service.faults_source = link_faults;
    link->service.faults_ctx = harness;
    link->service.logging_hz = log_hz;
    link->service.unlock_code = (unlock_code != NULL) ? unlock_code : "raccoon";
    link->log_hz = (log_hz == 0u) ? 1u : log_hz;
    rc_link_parser_init(&link->parser);
}

int rc_sim_link_listen(rc_sim_link_t *link, uint16_t port)
{
    struct sockaddr_in addr;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        (void)close(fd);
        return -1;
    }
    /* The LISTENING socket has to be non-blocking too, not just the accepted ones.
     *
     * accept() on a blocking listener returns only when somebody connects, so the service
     * loop parks inside it for as long as no tuner is attached - and the ECU stops
     * answering the moment nobody is watching. That is the worst possible default: a link
     * server that only works while someone is connected to it, discovered by the person who
     * connected and then watched it stop. */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    socklen_t len = (socklen_t)sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        link->port = ntohs(addr.sin_port);
    }
    link->listen_fd = fd;
    return 0;
}

uint16_t rc_sim_link_port(const rc_sim_link_t *link)
{
    return link->port;
}

int rc_sim_link_client_fd(const rc_sim_link_t *link)
{
    return link->client_fd;
}

/* Send a frame, or report that the link is gone.
 *
 * Returns false when the client could not take the data, and the caller drops it. The
 * alternative - a blocking send with no timeout - lets one client that has stopped reading
 * freeze the entire ECU: the send blocks, the service loop stops running, and every other
 * tuner sees a dead link. The symptom points at the network rather than at the one client
 * that stopped reading, which is why this is worth the extra return value.
 *
 * A real ECU has the same exposure on a stalled USB host, and the same answer: if the
 * buffer has been full for long enough, the link is not coming back. */
static bool send_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0u;
    while (sent < len) {
        const ssize_t n = send(fd, &data[sent], len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            stalled = 0u;
            continue;
        }
        if (n < 0 && (errno == EINTR)) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The peer's buffer is full. Give it a moment, then give up on it. */
            struct timespec pause;
            pause.tv_sec = 0;
            pause.tv_nsec = 2000000L;   /* 2 ms */
            (void)nanosleep(&pause, NULL);
            if (++stalled > 25u) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}


static void drop_client(rc_sim_link_t *link)
{
    if (link->client_fd >= 0) {
        (void)close(link->client_fd);
        link->client_fd = -1;
    }
    /* A dropped link is a dropped session: locked, with the guard's backoff reset. A client
     * that reconnects must unlock again, and must not inherit the previous session's
     * failures - otherwise a client that crashed and restarted would find itself locked
     * out for a backoff it never triggered. */
    link->service.authenticated = false;
    link->service.auth_failures = 0u;
    link->service.stage_received = 0u;
    link->service.stage_active = false;
    rc_unlock_guard_init(&link->service.guard, link->now_ms);
    rc_link_parser_init(&link->parser);
}

void rc_sim_link_tick(rc_sim_link_t *link, float dt_s, rc_harness_t *harness)
{
    /* Applied BEFORE the ECU is serviced, so the frame the tuner sees this tick already
     * reflects the engine being off. Applied after, the tuner would read one more frame
     * from a running engine after every "stop" and conclude the command did nothing. */
    if (link->engine_held_stopped) {
        harness->rider.starter_engaged = false;
        harness->rider.throttle_pct = 0.0f;
        harness->sim.running = false;
        harness->sim.rpm = 0.0f;
    }

    /* Time is virtual. The ECU runs in simulation time, so the link's own frame timeout and
     * the unlock backoff run on the same clock - otherwise a client that is merely slow
     * looks identical to one sending corruption. */
    link->now_us += (uint32_t)(dt_s * 1000000.0f);
    link->now_ms += (uint32_t)(dt_s * 1000.0f);
    link->service.guard.now_ms = link->now_ms;
    link->parser.now_us = link->now_us;

    /* One client at a time, and a new connection REPLACES the old one.
     *
     * This mirrors what a single-endpoint link actually does: a tuner that reconnects after
     * the application restarts is a new enumeration, and the old session is simply gone.
     * The alternative - keeping the first client and leaving the newcomer in the accept
     * backlog - looks like a working ECU to the first tuner and a dead link to the second,
     * and the second one's symptom (every request times out) points nowhere near the cause.
     */
    if (link->listen_fd >= 0) {
        const int fd = accept(link->listen_fd, NULL, NULL);
        if (fd >= 0) {
            if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
                (void)close(fd);
                return;
            }
            if (link->client_fd >= 0) {
                (void)close(link->client_fd);
                link->client_fd = -1;
            }
            const int one = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));
            /* Non-blocking, so a full peer buffer shows up as EAGAIN rather than parking
             * the whole ECU inside a send. */
            link->client_fd = fd;
            stalled = 0u;
            /* A new session starts locked. Reconnecting must never inherit an unlock: a
             * link that stays open across a reboot is exactly the case where that would
             * matter, and a test rig is where it is easiest to get wrong. */
            link->service.authenticated = false;
            link->service.auth_failures = 0u;
            link->service.stage_received = 0u;
            link->service.stage_active = false;
            /* Logging is a property of the SESSION, not of the ECU. A new client that
             * inherited the previous one's rate would either get a stream it never asked
             * for or - if the rate was left at zero - none at all, and a tuner whose
             * dashboard is blank would have no way to tell that from a dead link. */
            link->service.logging_hz = 0u;
            rc_unlock_guard_init(&link->service.guard, link->now_ms);
            rc_link_parser_init(&link->parser);
        }
    }
    if (link->client_fd < 0) {
        return;
    }

    /* Read whatever arrived, answer every complete frame in it, then emit live data at the
     * configured rate. Answering before emitting matters: a client that pipelines a request
     * behind the log stream should not wait a whole log interval for its reply. */
    uint8_t buffer[512];
    const ssize_t got = recv(link->client_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got == 0) {
        drop_client(link);
        return;
    }
    if (got > 0) {
        size_t offset = 0u;
        while (offset < (size_t)got) {
            rc_link_header_t header;
            uint8_t payload[RC_LINK_MAX_PAYLOAD];
            uint16_t length = 0u;
            size_t consumed = 0u;
            if (!rc_link_parse(&link->parser, &buffer[offset], (size_t)got - offset, &header,
                               payload, &length, &consumed)) {
                break;
            }
            offset += consumed;

            uint8_t response[RC_LINK_MAX_PAYLOAD + 8u];
            const uint16_t n = rc_link_service_handle(&link->service, &header, payload, length,
                                                      response, (uint16_t)sizeof(response));
            if (n > 0u) {
                rc_link_header_t reply;
                reply.sof = RC_LINK_SOF;
                reply.version = RC_LINK_VERSION;
                reply.command = (uint8_t)RC_CMD_RESPONSE;
                reply.flags = 0u;
                reply.sequence = header.sequence;   /* the client pairs replies by this */
                reply.length = n;
                uint8_t frame[RC_LINK_MAX_FRAME];
                const uint16_t flen = rc_link_encode(&reply, response, n, frame,
                                                     (uint16_t)sizeof(frame));
                if (!send_all(link->client_fd, frame, flen)) {
                    drop_client(link);
                    return;
                }
            }
        }
    }

    /* Live data. Emitted on a frame counter rather than a float accumulator: an accumulator
     * drifts with the step size, and a datalog whose sample interval wanders cannot be
     * integrated. */
    link->tick_count++;
    const uint32_t every = 1000u / link->log_hz;
    if (every == 0u || (link->tick_count % every) == 0u) {
        uint8_t frame[RC_LINK_MAX_FRAME];
        const uint16_t flen = rc_link_service_live_frame(&link->service, link->live_sequence++,
                                                         frame, (uint16_t)sizeof(frame));
        if (flen > 0u && !send_all(link->client_fd, frame, flen)) {
            drop_client(link);
            return;
        }
    }
    (void)harness;
}

int rc_sim_link_control(rc_sim_link_t *link, uint16_t port, rc_harness_t *harness)
{
    (void)harness;
    struct sockaddr_in addr;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        (void)close(fd);
        return -1;
    }
    socklen_t len = (socklen_t)sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        link->control_port = ntohs(addr.sin_port);
    }
    /* Non-blocking, and this matters more than it looks: the server loop calls
     * rc_sim_link_control_poll on every iteration, and a BLOCKING accept there parks the
     * whole simulator until somebody connects. The ECU then stops answering the tuner while
     * the rig's own control channel is idle - a hang that looks exactly like a dead link and
     * is fixed by closing a socket the operator never knew existed. */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    link->control_fd = fd;
    return 0;
}

uint16_t rc_sim_link_control_port(const rc_sim_link_t *link)
{
    return link->control_port;
}

/* One turn of the control port.
 *
 * Non-blocking throughout, on a connection that stays open. The earlier version accepted a
 * connection per command and then waited inside recv for the line to arrive, which parked
 * the ECU's service loop for as long as the wait was - and a wait that expires quietly
 * leaves the caller with no reply and no error, so a lost control command looks exactly
 * like a hung rig. Reading whatever has already arrived and acting on complete lines keeps
 * the loop's timing untouched and makes a partial command simply arrive a moment later. */
static void control_apply(rc_sim_link_t *link, rc_harness_t *harness, const char *line)
{
    if (strncmp(line, "stop", 4) == 0) {
        harness->rider.starter_engaged = false;
        harness->rider.throttle_pct = 0.0f;
        harness->rider.clutch_engaged = false;
        /* Both flags. rc_control_arm clears the CONTROL's armed bit, but the link service
         * asks the HARNESS whether the engine is running - so clearing only the control's
         * bit leaves every write refused, and "stop" appears not to work. */
        harness->armed = false;
        (void)rc_control_arm(&harness->control, false, NULL);
        harness->sim.running = false;
        harness->sim.rpm = 0.0f;
        link->engine_held_stopped = true;
        (void)send(link->control_client, "stopped\n", 8, MSG_NOSIGNAL);
    } else if (strncmp(line, "start", 5) == 0) {
        harness->rider.starter_engaged = true;
        harness->rider.clutch_engaged = true;
        harness->rider.gear = 0u;
        harness->rider.throttle_pct = 0.0f;
        link->engine_held_stopped = false;
        /* Re-arm as well as crank. "stop" disarmed the control, and a crank with the control
         * disarmed produces an engine that turns over and does nothing: the trigger never
         * syncs, nothing is scheduled, and the live frame reports an engine that is running
         * but not armed - which reads as an ECU fault rather than as a rig that was left
         * switched off. */
        harness->armed = true;
        (void)rc_control_arm(&harness->control, true, NULL);
        (void)send(link->control_client, "cranking\n", 9, MSG_NOSIGNAL);
    } else if (strncmp(line, "throttle", 8) == 0) {
        harness->rider.throttle_pct = (float)atof(&line[8]);
        (void)send(link->control_client, "ok\n", 3, MSG_NOSIGNAL);
    } else {
        (void)send(link->control_client, "unknown\n", 8, MSG_NOSIGNAL);
    }
}

void rc_sim_link_control_poll(rc_sim_link_t *link, rc_harness_t *harness)
{
    if (link->control_fd < 0) {
        return;
    }
    if (link->control_client < 0) {
        const int fd = accept(link->control_fd, NULL, NULL);
        if (fd < 0) { return; }
        if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) { (void)close(fd); return; }
        link->control_client = fd;
        link->control_used = 0u;
        link->control_taken = true;
    }

    for (;;) {
        char byte = 0;
        const ssize_t n = recv(link->control_client, &byte, 1u, MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0) {
                /* The rig's control console went away. Accept a new one rather than
                 * leaving a dead descriptor that silently swallows every later command. */
                (void)close(link->control_client);
                link->control_client = -1;
            }
            return;
        }
        if (byte == '\n' || byte == '\r') {
            if (link->control_used > 0u) {
                link->control_line[link->control_used] = '\0';
                control_apply(link, harness, link->control_line);
                link->control_used = 0u;
            }
            continue;
        }
        if (link->control_used + 1u < (uint16_t)sizeof(link->control_line)) {
            link->control_line[link->control_used++] = byte;
        }
        /* A command longer than the buffer is dropped at the newline rather than
         * truncated into something that might be valid: a truncated "throttle 100" is a
         * real throttle command, and running it by accident is worse than refusing it. */
    }
}

void rc_sim_link_close(rc_sim_link_t *link)
{
    drop_client(link);
    if (link->control_client >= 0) {
        (void)close(link->control_client);
        link->control_client = -1;
    }
    if (link->control_fd >= 0) {
        (void)close(link->control_fd);
        link->control_fd = -1;
    link->control_client = -1;
    }
    if (link->listen_fd >= 0) {
        (void)close(link->listen_fd);
        link->listen_fd = -1;
    }
}
