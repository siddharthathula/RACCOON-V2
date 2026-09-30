/* RACCOON V2 - the control cycle.
 *
 * This is where the spine of the product is visible in one function: requests are
 * computed, arbitration resolves them, and the hard safety envelope is applied LAST to
 * the command (docs/ENGINE_CONTROL_SPEC.md section 3).  Nothing downstream of step 19 can
 * raise a fuel or ignition value, and nothing upstream of it can see the envelope.
 *
 * Real-time discipline (FIRMWARE_ARCHITECTURE.md):
 *   - allocation-free, no blocking, no I/O beyond the HAL's O(1) entry points
 *   - every loop is bounded; the number of iterations is a function of the cylinder count,
 *     never of a measurement
 *   - this file is host-compilable, so the unit tests and the SIL run the shipping code
 */
#ifndef RC_ENGINE_CONTROL_H
#define RC_ENGINE_CONTROL_H

#include "cal_types.h"
#include "rc/engine.h"
#include "rc/fuel.h"
#include "rc/ignition.h"
#include "rc/idle.h"
#include "rc/performance.h"
#include "rc/rc_hal.h"
#include "rc/rc_types.h"
#include "rc/safety.h"
#include "rc/scheduler.h"
#include "rc/sensors.h"
#include "rc/torque.h"
#include "rc/trigger.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sensor channel indices, fixed by firmware so the tuner and the HAL agree. */
typedef enum {
    RC_IN_TPS1 = 0,
    RC_IN_TPS2 = 1,
    RC_IN_PEDAL1 = 2,
    RC_IN_PEDAL2 = 3,
    RC_IN_MAP = 4,
    RC_IN_MAF = 5,
    RC_IN_ECT = 6,
    RC_IN_IAT = 7,
    RC_IN_OIL_P = 8,
    RC_IN_OIL_T = 9,
    RC_IN_FUEL_P = 10,
    RC_IN_EGT = 11,
    RC_IN_ETHANOL = 12,
    RC_IN_BATTERY = 13,
    RC_IN_REF5V = 14,
    RC_IN_LAMBDA1 = 15,
    RC_IN_LAMBDA2 = 16,
    RC_IN_LAMBDA3 = 17,
    RC_IN_LAMBDA4 = 18,
    RC_IN_WHEEL_F = 19,
    RC_IN_WHEEL_R = 20,
    RC_IN_CHANNEL_COUNT = 21
} rc_input_channel_t;

/** The whole control system. */
typedef struct {
    rc_cal_t cal;
    rc_engine_t engine;
    rc_trigger_t trigger;
    rc_scheduler_t sched;
    rc_sensors_t sensors;
    rc_fuel_state_t fuel;
    rc_knock_state_t knock;
    /** Engine-cycle counter and the angle seen on the previous control tick.  The event
     *  plan is refreshed once per engine cycle, keyed on the WRAP of the cycle angle
     *  rather than on its value: keying on the value fails at the wrap, where the angle is
     *  legitimately both "less than last time" and "the same as last time" within one
     *  cycle, and the plan then either never refreshes or refreshes on every tick. */
    uint32_t cycle_count;
    uint16_t last_cycle_angle;
    uint32_t planned_cycle;
    rc_torque_manager_t torque;
    rc_safety_state_t safety;
    rc_idle_state_t idle;
    rc_perf_state_store_t perf;

    rc_engine_state_t state;   /* published, double buffered in the real system */
    uint32_t armed;
    uint32_t init_done;
    const char *arm_block_reason;
} rc_control_t;

/** Reset to a safe, disarmed state using the shipped defaults. */
void rc_control_init(rc_control_t *control);

/** Load a validated configuration; refuses an invalid one and leaves the ECU disarmed. */
bool rc_control_configure(rc_control_t *control, const rc_cal_t *cal, const char **reason);

/** Re-register every input channel against the current calibration.  Called by
 *  rc_control_configure, and separately by anything that replaces the calibration
 *  directly - a stale sensor table after a new tune is a fault that looks like a broken
 *  sensor and is not one. */
void rc_control_configure_inputs(rc_control_t *control);

/** Arm or disarm engine control, subject to the arming gate. */
bool rc_control_arm(rc_control_t *control, bool arm, const char **reason);

/** Feed a position-sensor edge. Called from the crank interrupt. */
void rc_control_on_capture(rc_control_t *control, uint8_t channel, uint32_t tick_us, uint8_t level);

/** Feed an analog frame. */
void rc_control_on_adc(rc_control_t *control, const rc_adc_frame_t *frame);

/**
 * Run one control cycle.
 *
 * Called once per 720 degrees, or on the bounded fallback tick.  Publishes the engine
 * state and schedules this cycle's events.
 */
void rc_control_cycle(rc_control_t *control, uint32_t now_us);

/** Dispatch due events and service the periodic work. Safe to call at any rate. */
/** Run the non-control-rate services.  `hal` receives the armed events; passing NULL runs
 *  the scheduler without wiring any output, which is only useful for state inspection. */
void rc_control_service(rc_control_t *control, uint32_t now_us, rc_hal_t *hal, void *hal_ctx);

/** Force every output safe. Used on reset, fault and the halted-CPU test. */
void rc_control_force_safe(rc_control_t *control, rc_hal_t *hal);

/** Effective engine time, integrated from the crank. */
uint32_t rc_control_engine_time(const rc_control_t *control);

#ifdef __cplusplus
}
#endif
#endif /* RC_ENGINE_CONTROL_H */
