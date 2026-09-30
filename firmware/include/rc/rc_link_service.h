/* RACCOON V2 - tuner link service: the command dispatcher.
 *
 * rc_link.c knows how to frame and unframe. This file knows what the frames MEAN, and it
 * is the only place in the firmware where a calibration can be changed by something other
 * than the control loop.
 *
 * THE RULES THIS FILE ENFORCES
 *
 * 1. A write requires authentication. Not "a write of a small value" or "a write while
 *    stationary" - a write. There is exactly one unlock path and every mutating command
 *    goes through it, because a rule with an exception is a rule that will eventually be
 *    used without one.
 *
 * 2. The engine must not be running. Refusing while armed is not caution, it is the only
 *    correct behaviour: a calibration that changes under a running engine can change the
 *    fuelling on the cycle it changed, which is how a bench tune becomes a track failure.
 *
 * 3. Nothing sent by a tuner is trusted. Every value is range-checked, the whole image is
 *    revalidated on the target, and the flash layer re-reads what it wrote. A tuner is an
 *    untrusted client even when it is the operator's own, because a compromised one is
 *    indistinguishable from an eager one.
 *
 * 4. Refusals explain themselves. A response carries the result code AND, for a validation
 *    failure, the rule that failed. "Rejected" with no reason teaches an operator nothing
 *    and gets worked around, and a workaround for a safety rule is how the safety rule
 *    gets removed.
 */
#ifndef RC_LINK_SERVICE_H
#define RC_LINK_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "cal_types.h"
#include "rc/rc_crypto.h"
#include "rc/rc_flash.h"
#include "rc/rc_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of salt the ECU issues. Part of the unlock exchange, not a secret: its job is to
 *  make a proof captured from one ECU useless against another. */
#define RC_LINK_SALT_SIZE 16u

/** The write buffer a STAGE is assembled into. Sized to hold a whole image plus room for
 *  the header so a chunked transfer never needs to be re-copied into a second buffer. */
#define RC_LINK_STAGE_BUFFER (RC_CAL_IMAGE_SIZE + 16u)

typedef struct {
    rc_hal_t *hal;
    void *hal_ctx;

    /** The working calibration. Reads and per-item writes land here, and this is what
     *  STAGE sends, so the tuner can build a tune cell by cell and stage it in one
     *  operation without ever sending the whole image twice. */
    rc_cal_t working;
    bool working_dirty;

    rc_flash_t *flash;
    rc_unlock_guard_t guard;

    uint8_t salt[RC_LINK_SALT_SIZE];
    /** The configured unlock code. Never leaves the ECU, and never crosses the wire: it is
     *  what both sides run through :func:`rc_unlock_code_derive` to get a key. NULL or empty
     *  means writes are impossible, which is a legitimate state for a ECU that has not been
     *  provisioned yet. */
    const char *unlock_code;
    bool authenticated;
    uint32_t auth_failures;

    /** Partial-transfer state. One at a time, by design: two interleaved transfers into
     *  one buffer is a bug waiting for a slow link to trigger it. */
    uint8_t stage_buffer[RC_LINK_STAGE_BUFFER];
    uint32_t stage_received;
    uint32_t stage_expected;
    bool stage_active;

    uint8_t read_buffer[RC_LINK_STAGE_BUFFER];
    uint32_t read_offset;
    bool read_active;

    uint16_t logging_hz;

    /** Live data, injected by the owner so this file does not need to know about the
     *  control loop. NULL means "no live data available", and GET_LIVE then says so. */
    const rc_engine_state_t *(*live_source)(void *ctx);
    void *live_ctx;
    bool (*armed_source)(void *ctx);
    void *armed_ctx;
    uint32_t (*faults_source)(void *ctx);
    void *faults_ctx;
} rc_link_service_t;

/** Prepare a service. `flash` may be NULL, in which case every flash command is refused
 *  with a storage error rather than pretending to succeed. */
void rc_link_service_init(rc_link_service_t *service, rc_hal_t *hal, void *hal_ctx,
                          rc_flash_t *flash);

/** Take the running image as the starting working copy, and refresh the flash view. */
void rc_link_service_adopt(rc_link_service_t *service, const rc_cal_t *running);

/** Where the service keeps the image the tuner is editing. */
rc_cal_t *rc_link_service_working(rc_link_service_t *service);

/**
 * Handle one inbound request.
 *
 * @return the number of bytes written to `response`, which ALWAYS begins with the result
 *         code. Never writes past `response_capacity`; a response that does not fit is
 *         truncated to a result-only reply rather than a partial payload the client would
 *         misparse.
 */
uint16_t rc_link_service_handle(rc_link_service_t *service,
                                const rc_link_header_t *header, const uint8_t *payload,
                                uint16_t length, uint8_t *response, uint16_t response_capacity);

/** Encode a live frame into a frame ready to send. Returns 0 when there is nothing to
 *  send, which is the normal case when logging is off. */
uint16_t rc_link_service_live_frame(rc_link_service_t *service, uint32_t sequence,
                                    uint8_t *out, uint16_t out_capacity);

/** True when the ECU would currently accept a mutating command. The tuner's UI asks this
 *  to grey out its write buttons with a reason rather than letting the operator find out
 *  by being refused. */
bool rc_link_service_write_allowed(rc_link_service_t *service, uint8_t *reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_LINK_SERVICE_H */
