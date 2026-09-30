/* RACCOON V2 - hard safety envelope.
 *
 * The last thing that touches a fuel or ignition command (spec section 110), applied to
 * the *command* rather than to the request, so a feature bug, a tuner mistake or an
 * engineering-mode session cannot weaken it.  Engineering mode may only *narrow* an
 * envelope, never widen it.
 *
 * Structural argument (SAFETY_CASE.md section 5):
 *   1. The envelope runs after the torque manager and is not an input to it.
 *   2. Race modes select calibration overlays, not code paths; the limits live in the
 *      base image and are therefore global.
 *   3. Hardware protections (max dwell, duty ceiling, overspeed trip) are independent of
 *      the CPU and survive a halted core.
 * That is why the envelope is a separate, last, non-bypassable function rather than a
 * branch inside the fuel or ignition code.
 */
#ifndef RC_SAFETY_H
#define RC_SAFETY_H

#include "cal_types.h"
#include "rc/engine.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Everything the envelope needs to judge. */
typedef struct {
    float rpm;
    float lambda;
    uint8_t lambda_valid;
    float ect_c;
    uint8_t ect_valid;
    float iat_c;
    uint8_t iat_valid;
    float oil_pressure_bar;
    uint8_t oil_pressure_valid;
    float oil_temp_c;
    uint8_t oil_temp_valid;
    float egt_c;
    uint8_t egt_valid;
    float battery_v;
    uint8_t battery_valid;
    float fuel_pressure_kpa;
    uint8_t fuel_pressure_valid;
    float injector_duty_percent;
    float dwell_ms;
    float boost_kpa;
    float speed_kph;
    uint8_t sync_lost;
    uint8_t sensor_fault;
    uint8_t sync_state;
} rc_safety_input_t;

/** The envelope's verdict. */
typedef struct {
    uint32_t active;          /**< RC_SAFETY_* bitmask */
    rc_action_t action;       /**< most severe active action */
    float torque_factor;      /**< 0..1, multiplies the fuel request */
    float ignition_max_deg;   /**< clamped advance ceiling for this cycle */
    float ignition_min_deg;
    bool fuel_cut;
    bool spark_cut;
    const char *first_reason;
} rc_safety_output_t;

typedef struct {
    /* Latches, per hazard. Each has a documented release predicate; none may be released
     * by another subsystem (T3 in the torque manager, and the same idea here). */
    bool lean_latched;
    bool oil_latched;
    bool sync_latched;
    uint32_t lean_since_us;
    uint32_t oil_since_us;
    uint32_t lean_hold_us;
    uint32_t oil_hold_us;
    uint32_t last_reset_us;
    uint32_t last_action;
} rc_safety_state_t;

void rc_safety_init(rc_safety_state_t *state);

/**
 * Evaluate the envelope and produce the factors the rest of the pipeline applies.
 *
 * Pure function of (state, input, calibration): no I/O, no allocation, O(1) apart from
 * the latch bookkeeping, so it is exhaustively testable and fuzzable.
 */
void rc_safety_evaluate(rc_safety_state_t *state, const rc_cal_t *cal, const rc_engine_t *engine,
                        const rc_safety_input_t *in, uint32_t now_us, rc_safety_output_t *out);

/** Release the latched hazards whose recovery predicates are satisfied. */
void rc_safety_service_latches(rc_safety_state_t *state, const rc_cal_t *cal,
                               const rc_safety_input_t *in, uint32_t now_us);

/** Reason string for an active bitmask, for the "fault explanation" view. */
const char *rc_safety_reason(uint32_t active);

#ifdef __cplusplus
}
#endif
#endif /* RC_SAFETY_H */
