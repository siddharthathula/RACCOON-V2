/* RACCOON V2 - idle control. See idle.h. */
#include "rc/idle.h"

#include <string.h>

#include "cal_access.h"

void rc_idle_init(rc_idle_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void rc_idle_update(rc_idle_state_t *state, const rc_cal_t *cal, float rpm, float target_rpm,
                    float tps_pct, float ect_c, float torque_allowance, uint32_t now_us,
                    rc_idle_output_t *out)
{
    if (state == NULL || cal == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->strategy = (uint8_t)cal->idle_idle_strategy;

    const float entry = rc_cal_get_idle_idle_entry_rpm(cal);
    const float exit = rc_cal_get_idle_idle_exit_rpm(cal);
    /* Hysteresis on the entry/exit speeds is what stops the idle valve hunting around the
     * threshold, which on a real valve sounds and feels like a misfire. */
    const bool throttle_closed = (tps_pct <= 1.0f);
    const bool warm = (ect_c >= rc_cal_get_idle_idle_enable_at_ect_c(cal));
    out->engaged = (rpm < exit) && (rpm < (entry + 200.0f)) && throttle_closed && warm;
    if (torque_allowance < 0.5f) {
        /* The torque manager has already reduced authority; idle must not fight it. */
        out->engaged = false;
    }
    if (out->strategy == (uint8_t)RC_IDLE_OFF) {
        /* A strategy of "off" means the tuner has switched idle off, so the module must
         * report itself disengaged.  Reporting "engaged" while commanding nothing would
         * leave the rest of the ECU believing an idle loop is running that is not. */
        out->engaged = false;
    }

    switch (out->strategy) {
        case RC_IDLE_PWM_IAC: {
            if (!out->engaged) {
                /* Return to a defined minimum rather than to zero, so a restart does not
                 * need a long open-loop cranking period. */
                out->valve_pct = rc_cal_get_idle_idle_valve_duty_min_pct(cal);
                out->valve_pwm_duty = out->valve_pct;
                state->integrator = 0.0f;
                break;
            }
            const float error = target_rpm - rpm;
            const float gain = rc_cal_get_idle_idle_closed_loop_gain(cal);
            state->integrator += error * gain * 0.001f;
            const float max_duty = rc_cal_get_idle_idle_valve_duty_max_pct(cal);
            const float min_duty = rc_cal_get_idle_idle_valve_duty_min_pct(cal);
            state->integrator = rc_clampf(state->integrator, min_duty, max_duty);
            out->valve_pct = state->integrator;
            out->valve_pwm_duty = state->integrator;
            break;
        }
        case RC_IDLE_STEPPER: {
            if (!out->engaged) {
                state->stepper_energised = false;
                break;
            }
            const float error = target_rpm - rpm;
            const int32_t step = (int32_t)rc_clampf(error * 0.01f, -1.0f, 1.0f);
            const int32_t steps_max = (int32_t)rc_cal_get_idle_idle_stepper_steps(cal);
            state->stepper_position = rc_clampi32(state->stepper_position + step, 0,
                                                  (steps_max > 0) ? steps_max : 0);
            state->stepper_energised = true;
            if (state->energised_since_us == 0u) {
                state->energised_since_us = now_us;
            }
            /* A stepper that is commanded forever is a thermal event, so the maximum
             * energised time is a hardware-adjacent bound, not a suggestion. */
            const uint32_t limit_ms = rc_cal_get_idle_idle_stepper_max_energised_ms(cal);
            if (limit_ms > 0u
                && rc_ticks_forward(now_us, state->energised_since_us) > (limit_ms * 1000u)) {
                state->stepper_energised = false;
                state->faulted = true;
            }
            out->stepper_energised = state->stepper_energised;
            out->stepper_position = (uint16_t)state->stepper_position;
            break;
        }
        case RC_IDLE_IGNITION:
            /* Ignition-only idle retards the spark slightly to stabilise a slow idle; it
             * is a correction, never a substitute for a valve. */
            out->ignition_bis_deg = out->engaged ? -3.0f : 0.0f;
            break;
        case RC_IDLE_DBW:
        case RC_IDLE_OFF:
        default:
            break;
    }
}
