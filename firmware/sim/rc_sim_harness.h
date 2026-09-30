/* RACCOON V2 - software-in-the-loop harness.
 *
 * This is the closed loop.  A simulated engine and a real ECU, wired together exactly the
 * way a motorcycle wires them: the crank turns because the engine made torque, the ECU
 * sees the sensors, it schedules injector and coil events, and the engine consumes them.
 * Nothing is stubbed in the middle.
 *
 * The harness owns the time base and the event loop, because that ordering is the thing
 * under test.  A unit test can hand the ECU a perfect crank angle; only a closed loop
 * shows what the ECU does when the crank arrives late, when a pulse is shorter than
 * injector dead time, or when the sensor is lying.
 */
#ifndef RC_SIM_HARNESS_H
#define RC_SIM_HARNESS_H

#include "rc/engine_control.h"
#include "rc/hal_virtual.h"
#include "rc_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_HARNESS_MAX_PENDING 32u
#define RC_HARNESS_CYLINDERS RC_SIM_CYLINDERS

/** One event the ECU armed, waiting for the simulated timer to reach it. */
typedef struct {
    rc_event_t event;
    bool used;
} rc_harness_pending_t;

/** Everything the harness knows. */
typedef struct {
    rc_control_t control;
    rc_hal_virtual_t hal;
    rc_sim_t sim;
    rc_sim_rider_t rider;
    rc_sim_faults_t faults;

    /* Configuration, so a scenario can describe an engine rather than hard-code it. */
    uint8_t ckp_teeth;
    uint8_t ckp_missing;
    uint8_t cmp_teeth;

    /* Derived each step. */
    float step_us;
    float dt_s;
    uint32_t adc_sequence;

    rc_harness_pending_t pending[RC_HARNESS_MAX_PENDING];
    uint16_t pending_count;
    /** Stop feeding crank edges while the plant keeps turning.  This is a trigger wire
     *  that has come off, which is NOT the same as a stopped engine - and it is the only
     *  way to test the decoder watchdog, because a stopped engine produces no edges for a
     *  reason the decoder can see. */
    bool suppress_trigger;

    /* Observability, so a scenario can assert on behaviour rather than on internals. */
    uint32_t sim_steps;
    uint32_t events_fired;
    uint32_t events_dropped;   /**< armed after the queue was full */
    uint32_t events_by_type[RC_EV_TYPE_COUNT];
    uint32_t cycles_run;
    float last_lambda;
    float last_rpm;
    float last_ignition_deg;
    float last_pulse_ms;
    bool armed;
    bool saw_lean_cut;
    bool saw_fuel_cut;
    bool saw_sync_loss;
    bool saw_oil_trip;
    bool non_finite_seen;     /**< set if any published value was NaN or Inf */
    /** Why the last rc_harness_set_cal was refused, or NULL.  A scenario asserting that a
     *  bad tune is rejected needs the reason, not just the failure. */
    const char *configure_reason;
} rc_harness_t;

/** A tune that matches a given engine geometry, derived from the shipped defaults.
 *
 *  Public because the trigger profile, edge polarity and cam tooth count in the shipped
 *  defaults describe a different engine, and a harness built without this will not arm. The
 *  polarity in particular is part of describing the VEHICLE: get it wrong and the plant
 *  emits edges the ECU correctly ignores, so the engine never starts and nothing points at
 *  the cause. */
void rc_harness_default_tune(uint8_t cylinders, uint8_t ckp_teeth, uint8_t ckp_missing,
                             uint8_t cmp_teeth, float swept_cc, rc_cal_t *out);

/** Build a harness for an engine of the given geometry, with a tune that matches it. */
void rc_harness_init(rc_harness_t *h, float swept_cc, uint8_t cylinders, uint8_t ckp_teeth,
                     uint8_t ckp_missing, uint8_t cmp_teeth);

/** Replace the tune.  The harness does not validate it: a scenario deliberately feeding an
 *  invalid configuration is a test, not a mistake. */
void rc_harness_set_cal(rc_harness_t *h, const rc_cal_t *cal);

/** Advance one step: crank edges, ADC frame, control cycle, scheduled events, plant. */
void rc_harness_step(rc_harness_t *h, float dt_s);

/** Run for `seconds` of simulated time. */
void rc_harness_run(rc_harness_t *h, float seconds, float dt_s);

/** Arm or disarm the ECU as the rider would with the key. */
void rc_harness_set_armed(rc_harness_t *h, bool armed);

#ifdef __cplusplus
}
#endif
#endif /* RC_SIM_HARNESS_H */
