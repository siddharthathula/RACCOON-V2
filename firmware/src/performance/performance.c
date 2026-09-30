/* RACCOON V2 - performance features as torque requesters. See performance.h. */
#include "rc/performance.h"

#include <string.h>

#include "cal_access.h"

void rc_perf_init(rc_perf_state_store_t *store)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->state = RC_PERF_IDLE;
}

float rc_perf_slip_percent(float front_kph, float rear_kph)
{
    /* Slip is the difference normalised by the *driven* wheel, so it is well defined from
     * a standstill. A zero driven-wheel speed would divide by zero, so the guarded
     * division returns zero, i.e. "no slip information", which is the honest answer. */
    return rc_safe_div(front_kph - rear_kph, rc_absf(rear_kph), 0.0f) * 100.0f;
}

void rc_perf_update(rc_perf_state_store_t *store, const rc_cal_t *cal, rc_torque_manager_t *tm,
                    const rc_perf_input_t *in, uint32_t now_us, rc_perf_output_t *out)
{
    if (store == NULL || cal == NULL || tm == NULL || in == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = store->state;
    out->launch_torque_scale = 1.0f;
    out->shift_torque_scale = 1.0f;
    out->traction_torque_scale = 1.0f;

    /* ------------------------------------------------------------ pit limiter */
    if (cal->pit_pit_limiter_enabled != 0u && in->pit_requested) {
        const float target_speed = rc_cal_get_pit_pit_target_speed_kph(cal);
        const float target_rpm = rc_cal_get_pit_pit_target_rpm(cal);
        const bool over = (in->speed_kph > target_speed) || (in->rpm > target_rpm);
        if (over) {
            store->pit_latched = true;
            out->state = RC_PERF_PIT_LIMIT;
        }
    }
    if (store->pit_latched != false) {
        /* Hysteresis, so releasing the switch does not immediately resume full power at
         * the limit speed. */
        const float target_speed = rc_cal_get_pit_pit_target_speed_kph(cal);
        const float release = target_speed - rc_absf(rc_cal_get_pit_pit_speed_hysteresis_kph(cal));
        if (in->speed_kph < release) {
            store->pit_latched = false;
        } else {
            rc_torque_request(tm, RC_REQ_SHIFT, 0.0f, true, now_us, 0u);
            out->state = RC_PERF_PIT_LIMIT;
            return;
        }
    }

    /* ------------------------------------------------------------- launch */
    if (rc_cal_get_launch_launch_enabled(cal) != 0u) {
        const float target_rpm = rc_cal_get_launch_launch_target_rpm(cal);
        const uint8_t shift_source = cal->strategy_shift_request_source;
        const bool can_launch = (shift_source != 0u) || in->clutch_pressed;
        if (in->launch_requested && can_launch && !store->launch_latched) {
            store->launch_latched = true;
            store->launch_until_us = now_us + (rc_cal_get_launch_launch_timeout_ms(cal) * 1000u);
            out->launch_events++;
        }
        if (store->launch_latched != false) {
            /* "Has the deadline passed" is a deadline comparison, not a magnitude one.
             * `rc_ticks_forward(now, until)` is now-minus-until, so before the deadline it
             * wraps to a huge positive number - which a `> 0` test reads as "expired".  A
             * launch would therefore time out on the very cycle it started, and no launch
             * could ever hold.  rc_deadline_ok() is the wrap-safe form. */
            const bool timed_out = !rc_deadline_ok(now_us, store->launch_until_us);
            if (timed_out || !in->launch_requested) {
                store->launch_latched = false;
            } else if (in->rpm < target_rpm) {
                /* Hold the engine at the target: the launch strategy *requests* a
                 * reduced torque so the rev limiter does the holding, which is simpler and
                 * more repeatable than trying to meter fuel at the limiter. */
                const float ramp = rc_clampf(in->dt_s * 10.0f, 0.0f, 1.0f);
                const float scale = rc_cal_get_launch_launch_torque_scale(cal);
                (void)ramp;
                rc_torque_request(tm, RC_REQ_LAUNCH, scale, false, now_us, 0u);
                out->launch_torque_scale = scale;
                out->state = RC_PERF_LAUNCH_ACTIVE;
            } else {
                out->state = RC_PERF_LAUNCH_ARMED;
            }
        }
    }

    /* -------------------------------------------------- flat shift / blip */
    if (rc_cal_get_shift_shift_blip_enabled(cal) != 0u && in->shift_requested
        && in->rpm > rc_cal_get_shift_shift_min_rpm(cal)) {
        /* Auto-blip raises engine speed on a downshift to match the next gear.  On a cable
         * throttle the only levers are ignition and fuel, and pretending otherwise would
         * be a lie in the calibration. */
        const float blip_scale = rc_cal_get_shift_shift_blip_torque_scale(cal);
        const uint32_t duration = rc_cal_get_shift_shift_blip_duration_ms(cal);
        rc_torque_request(tm, RC_REQ_RIDER, blip_scale, false, now_us, 0u);
        store->shift_until_us = now_us + (duration * 1000u);
        store->shift_active = true;
        out->state = RC_PERF_SHIFT;
    } else if (in->shift_requested && in->clutch_pressed
               && in->rpm > rc_cal_get_shift_shift_min_rpm(cal)) {
        const uint32_t max_cut = rc_cal_get_shift_shift_max_cut_ms(cal);
        store->shift_until_us = now_us + (max_cut * 1000u);
        store->shift_active = true;
        out->state = RC_PERF_SHIFT;
        out->shift_events++;
    }
    /* The recovery window applies only to a shift that actually happened.  Testing a bare
     * timestamp would make the window look "already expired" at boot - now > 0 is true
     * before any shift - so a shift-recovery torque request would be issued continuously
     * from power-on until the first upshift, quietly holding the engine back at idle. */
    if (store->shift_active != false && !rc_deadline_ok(now_us, store->shift_until_us)) {
        const float recovery = rc_cal_get_shift_shift_recovery_torque_scale(cal);
        rc_torque_request(tm, RC_REQ_SHIFT, recovery, false, now_us, 0u);
        out->shift_torque_scale = recovery;
    }

    /* -------------------------------------------------- traction control */
    if (rc_cal_get_traction_traction_enabled(cal) != 0u
        && cal->strategy_wheel_speed_channel_count >= 2u) {
        const float slip = rc_perf_slip_percent(in->front_speed_kph, in->rear_speed_kph);
        out->slip_percent = slip;
        const float target = rc_cal_get_traction_traction_slip_target_pct(cal);
        const float minimum = rc_cal_get_traction_traction_min_slip_to_act(cal);
        if (slip > target && slip > minimum && in->speed_kph > minimum) {
            const float over = rc_safe_div(slip - target, target, 0.0f);
            const float gain = rc_cal_get_traction_traction_gain(cal);
            const float scale = rc_clampf(1.0f - (over * gain), 0.0f, 1.0f);
            rc_torque_request(tm, RC_REQ_TRACTION, scale, false, now_us, 0u);
            out->traction_torque_scale = scale;
            out->traction_events++;
            out->state = RC_PERF_TRACTION;
        }
    }

    /* ----------------------------------------------------- engine braking */
    if (rc_cal_get_ebt_engine_braking_enabled(cal) != 0u && in->throttle_closed
        && in->rpm > rc_cal_get_ebt_ebt_min_rpm(cal)) {
        const float retard = rc_table2d_interp_on(
            cal->ebt_ebt_retard_table_cells, RC_CELL_F32, 8u, 8u,
            cal->ebt_ebt_retard_table_gear_axis_cells, cal->ebt_ebt_retard_table_rpm_axis_cells,
            RC_CELL_U16, (float)in->gear, in->rpm, 1.0f);
        out->engine_braking_retard_deg = retard;
        /* Engine braking is a small authority *increase* relative to the idle target, not
         * a reduction, so it is reported separately and the ignition module applies it. */
    }

    /* ------------------------------------------------ wheelie mitigation */
    if (rc_cal_get_launch_launch_wheelie_mitigation_enabled(cal) != 0u
        && store->launch_latched != false) {
        out->wheelie_mitigating = true;
    }
}
