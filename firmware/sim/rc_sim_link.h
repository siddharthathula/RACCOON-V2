/* RACCOON V2 - simulator link server. See rc_sim_link.c for why this is the real ECU and
 * not a mock.
 */
#ifndef RC_SIM_LINK_H
#define RC_SIM_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/rc_flash.h"
#include "rc/rc_link.h"
#include "rc/rc_link_service.h"
#include "rc_sim_harness.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Big enough for two calibration slots plus the activation marker, so the flash layer runs
 * its real sector logic rather than a special case. */
#define RC_SIM_LINK_MEDIUM_SIZE 0x30000u

typedef struct {
    rc_hal_t hal;
    rc_flash_t flash;
    rc_link_service_t service;
    rc_link_parser_t parser;
    int listen_fd;
    int client_fd;
    int control_fd;
    int control_client;
    char control_line[64];
    uint16_t control_used;
    uint16_t port;
    uint16_t control_port;
    /** Set once the control port has issued a command, after which the server stops
     *  managing the rider itself. Two things driving the throttle at once produces an engine
     *  that responds to neither. */
    bool control_taken;
    /** Set by "stop" and cleared by "start". While set, the server forces the plant off
     *  after every step.
     *
     *  Zeroing the rpm once is not enough: the plant integrates its own inertia, so a
     *  spinning crank keeps spinning and the engine is still turning while the ECU has
     *  been told it is stopped. That inconsistency is worse than either state on its own -
     *  it means "the engine is off" and "the ECU is writing a tune" are true at the same
     *  time. */
    bool engine_held_stopped;
    uint16_t log_hz;
    uint32_t tick_count;
    uint32_t live_sequence;
    uint32_t now_us;
    uint32_t now_ms;
    uint8_t medium[RC_SIM_LINK_MEDIUM_SIZE];
} rc_sim_link_t;

/** Prepare the server against an already-initialised harness. `log_hz` of 0 means no live
 *  data, which is a legitimate configuration and not an error. `unlock_code` is the code
 *  the ECU will accept; NULL or "" means writes are impossible, which is a real state and
 *  is worth being able to run. */
void rc_sim_link_init(rc_sim_link_t *link, rc_harness_t *harness, uint16_t log_hz,
                      const char *unlock_code);

/** Bind to loopback. Port 0 asks the OS for a free one, which is what the tests and the
 *  GUI both want - a hard-coded port is a hard-coded "address already in use". */
int rc_sim_link_listen(rc_sim_link_t *link, uint16_t port);

/** The port actually bound, valid after a successful :func:`rc_sim_link_listen`. */
uint16_t rc_sim_link_port(const rc_sim_link_t *link);

/** The connected client, or -1. Public so the server loop can wait on it. */
int rc_sim_link_client_fd(const rc_sim_link_t *link);

/** One turn: advance the virtual clock, accept, answer, and emit live data. */
void rc_sim_link_tick(rc_sim_link_t *link, float dt_s, rc_harness_t *harness);

/**
 * Open a plain-text control port, one command per line.
 *
 * This is deliberately NOT part of the tuner protocol. Starting and stopping an engine is
 * something an operator does with a key, not something a tune tool does over the link -
 * putting it in the protocol would mean a connected client could stop a running motorcycle,
 * which is precisely the thing the NOT_ARMED gate exists to prevent. The control port is a
 * test-rig affordance, it exists only in the simulator, and it is how the GUI can be
 * exercised through a full workflow: crank, run, stop, then flash.
 *
 * Commands: "stop" (engine off), "start" (crank then idle), "throttle <0..100>".
 */
int rc_sim_link_control(rc_sim_link_t *link, uint16_t port, rc_harness_t *harness);

/** The bound control port, valid after a successful rc_sim_link_control. */
uint16_t rc_sim_link_control_port(const rc_sim_link_t *link);

/** One turn of the control port: accept a command line, act, reply. */
void rc_sim_link_control_poll(rc_sim_link_t *link, rc_harness_t *harness);

void rc_sim_link_close(rc_sim_link_t *link);

#ifdef __cplusplus
}
#endif
#endif /* RC_SIM_LINK_H */
