/* RACCOON V2 - idle control.
 *
 * Strategy-gated: a PWM valve, a stepper or ignition-only idle.  A stepper is a thermal
 * hazard if it is commanded forever, so it has a hard maximum energised time and a
 * return-to-zero policy (ACTUATOR_SPEC.md section 4).
 */
#ifndef RC_IDLE_H
#define RC_IDLE_H

#include "cal_types.h"
#include "rc/rc_math.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RC_IDLE_OFF = 0,
    RC_IDLE_PWM_IAC = 1,
    RC_IDLE_STEPPER = 2,
    RC_IDLE_DBW = 3,
    RC_IDLE_IGNITION = 4
} rc_idle_strategy_t;

typedef struct {
    bool engaged;
    float valve_pct;            /**< requested valve opening, percent */
    float valve_pwm_duty;       /**< commanded duty after the min/max clamp */
    int32_t stepper_position;   /**< steps from the closed position */
    bool stepper_energised;
    bool anti_stall;
    float ignition_bis_deg;
    uint8_t strategy;
} rc_idle_output_t;

typedef struct {
    float integrator;
    int32_t stepper_position;   /**< working position; wider than the published field */
    bool stepper_energised;
    uint32_t energised_since_us;
    bool faulted;
} rc_idle_state_t;

void rc_idle_init(rc_idle_state_t *state);

void rc_idle_update(rc_idle_state_t *state, const rc_cal_t *cal, float rpm, float target_rpm,
                    float tps_pct, float ect_c, float torque_allowance, uint32_t now_us,
                    rc_idle_output_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RC_IDLE_H */
