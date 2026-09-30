/* RACCOON V2 - ignition, dwell and knock. See ignition.h for rules I1..I5. */
#include "rc/ignition.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"

void rc_ignition_init(rc_knock_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    for (uint8_t i = 0u; i < RC_MAX_CYLINDERS; ++i) {
        state->noise_floor[i] = rc_clampf(0.0f, 0.0f, 0.0f);
    }
}

float rc_ignition_dwell_ms(const rc_cal_t *cal, float battery_v, float rpm)
{
    /* Battery x RPM table: the coil's primary inductance is the physical constraint, so
     * the table is a measurement, and the model is "reach the target current in the
     * available time". */
    const float table = rc_table2d_interp_on(
        cal->ignition_dwell_table_cells, RC_CELL_F32, 8u, 8u,
        cal->ignition_dwell_table_volt_axis_cells, cal->ignition_dwell_table_rpm_axis_cells,
        RC_CELL_U16, battery_v, rpm, 1.0f);
    /* I3: software bounds, and the hardware ceiling is a fourth bound. */
    const float soft_min = rc_cal_get_ignition_dwell_min_ms(cal);
    const float soft_max = rc_cal_get_ignition_dwell_max_ms(cal);
    const float hardware_max = rc_cal_get_hardware_ignition_max_dwell_ms(cal);
    const float ceiling = rc_minf(soft_max, hardware_max);
    if (ceiling < soft_min) {
        /* A configuration where the soft maximum is above the hardware ceiling is a
         * configuration error; the build-time rule dwell_max_within_hardware catches it,
         * and this is the local belt-and-braces clamp. */
        return soft_min;
    }
    return rc_clampf(table, soft_min, ceiling);
}

float rc_ignition_correction_deg(const rc_cal_t *cal, const rc_ignition_input_t *in)
{
    /* The order is normative and matches docs/IGNITION_SPEC.md section 2.  A change here
     * is a calibration-visible behaviour change, so it is documented, not incidental. */
    float sum = 0.0f;

    /* Intake air temperature: warm air needs less advance. */
    const float iat_span = in->iat_c - 20.0f;
    sum += rc_clampf(iat_span * rc_cal_get_ignition_ignition_iat_correction_deg(cal),
                     -20.0f, 20.0f);

    /* Engine temperature: warm engine, less advance.  Bounded, because a failed ECT
     * must not be able to command 40 degrees of retard. */
    const float ect_span = in->ect_c - 40.0f;
    sum += rc_clampf(ect_span * rc_cal_get_ignition_ignition_ect_correction_deg(cal),
                     -25.0f, 25.0f);

    /* Battery: a low-voltage coil cannot sustain the commanded dwell, so advance is cut. */
    const float batt_threshold = rc_cal_get_ignition_ignition_battery_retard_below_v(cal);
    if (in->battery_v < batt_threshold) {
        const float deficit = batt_threshold - in->battery_v;
        sum -= rc_clampf(deficit * rc_cal_get_ignition_ignition_battery_retard_deg(cal),
                         0.0f, 15.0f);
    }

    /* Altitude: less pressure means a leaner burn, which tolerates more advance. */
    const float baro_delta = (100.0f - in->baro_kpa) / 100.0f;
    sum += rc_clampf(baro_delta * 2.0f, -6.0f, 6.0f);

    /* Fuel: ethanol charge cooling needs a positive shift, bounded by the calibration. */
    const float blend = rc_clampf(in->ethanol_fraction, 0.0f, 1.0f);
    sum += rc_clampf(blend * rc_cal_get_flexfuel_flexfuel_ignition_blend_deg(cal),
                     -rc_cal_get_flexfuel_flexfuel_ignition_blend_max_deg(cal),
                     rc_cal_get_flexfuel_flexfuel_ignition_blend_max_deg(cal));

    /* High-RPM retard. */
    if (in->rpm > 6000.0f) {
        sum -= rc_clampf((in->rpm - 6000.0f) / 1000.0f
                             * rc_cal_get_ignition_ignition_high_rpm_retard(cal),
                         0.0f, 12.0f);
    }

    /* User trim, bounded by the calibration. */
    const float trim_max = rc_cal_get_ignition_ignition_user_trim_max_deg(cal);
    sum += rc_clampf(in->user_trim_deg, -trim_max, trim_max);

    return sum;
}

void rc_ignition_update(const rc_cal_t *cal, const rc_engine_t *engine,
                        const rc_knock_output_t *knock, const rc_ignition_input_t *in,
                        const bool *external_cut, rc_ignition_output_t *out)
{
    if (cal == NULL || engine == NULL || in == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* I2: the hard envelope, read once, applied last. */
    out->envelope_min_deg = rc_cal_get_strategy_ignition_hard_min_deg(cal);
    out->envelope_max_deg = rc_cal_get_strategy_ignition_hard_max_deg(cal);

    out->correction_deg = rc_ignition_correction_deg(cal, in);

    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        if (cyl >= engine->cylinder_count) {
            out->final_deg[cyl] = 0.0f;
            out->cut[cyl] = 1u;
            continue;
        }
        /* The firing angle decides which row and column of the table this cylinder uses,
         * which is what lets one map serve a 180 degree twin and an inline four. */
        const float firing = engine->firing_angle_deg[cyl];
        const float base = rc_table2d_interp_on(
            cal->ignition_ignition_base_table_cells, RC_CELL_F32, 16u, 16u,
            cal->ignition_ignition_base_table_rpm_axis_cells,
            cal->ignition_ignition_base_table_load_pct_axis_cells, RC_CELL_U16, in->rpm,
            in->load, 1.0f);
        out->base_deg[cyl] = base;
        (void)firing;

        float total = base + out->correction_deg;
        if (knock != NULL && cyl < RC_MAX_CYLINDERS) {
            total -= rc_absf(knock->retard_deg[cyl]);
            out->knock_retard_deg[cyl] = knock->retard_deg[cyl];
        }

        if (total < out->envelope_min_deg || total > out->envelope_max_deg) {
            out->envelope_limited = true;
        }
        out->final_deg[cyl] = rc_clampf(total, out->envelope_min_deg, out->envelope_max_deg);
        out->dwell_ms[cyl] = rc_ignition_dwell_ms(cal, in->battery_v, in->rpm);

        /* I4: a cut is a flag on the output, and control.c turns it into a scheduler
         * suppression. It is never expressed as an angle of zero. */
        out->cut[cyl] = (external_cut != NULL && external_cut[cyl] != false) ? 1u : 0u;
    }
}

void rc_knock_update(rc_knock_state_t *state, const rc_cal_t *cal, uint8_t cylinder,
                     float sample, float window_open_fraction)
{
    if (state == NULL || cal == NULL || cylinder >= RC_MAX_CYLINDERS) {
        return;
    }
    if (rc_cal_get_knock_knock_enabled(cal) == 0u
        || rc_cal_get_strategy_knock_sensor_count(cal) == 0u) {
        /* I0: no sensor, no knock claim. The retard stays at zero and the feature is
         * reported as unavailable rather than pretending to be quiet. */
        return;
    }

    /* A slow-decaying noise floor, so a disconnected sensor cannot masquerade as either
     * "no knock" or "constant knock". */
    const float floor_decay = 0.999f;
    if (sample < state->noise_floor[cylinder] * floor_decay) {
        state->noise_floor[cylinder] = sample;
    } else {
        state->noise_floor[cylinder] += (sample - state->noise_floor[cylinder]) * 0.001f;
    }
    const float threshold = rc_cal_get_knock_knock_threshold(cal)
        + (state->noise_floor[cylinder] * rc_cal_get_knock_knock_noise_floor(cal));
    if (sample < threshold) {
        return;
    }

    /* Only an event inside the window counts; combustion noise elsewhere is not knock. */
    if (!(window_open_fraction > 0.0f)) {
        return;
    }
    state->event_count[cylinder]++;
    state->knock_events++;
    if (state->event_count[cylinder] >= 255u) {
        state->event_count[cylinder] = 255u;
    }

    const float per_event = rc_cal_get_knock_knock_retard_per_event_deg(cal);
    const float max_total = rc_cal_get_knock_knock_max_total_retard_deg(cal);
    float retard = state->retard_deg[cylinder] + per_event;

    /* Escalation: a second threshold for a bigger jump, then a persistent state. */
    if (state->event_count[cylinder] >= rc_cal_get_knock_knock_escalation_3_events(cal)) {
        retard += rc_cal_get_knock_knock_escalation_2_retard_deg(cal);
    }
    state->retard_deg[cylinder] = rc_clampf(retard, 0.0f, rc_absf(max_total));
}

void rc_knock_decay(rc_knock_state_t *state, const rc_cal_t *cal, float dt_s)
{
    if (state == NULL || cal == NULL || dt_s <= 0.0f) {
        return;
    }
    /* I5: bounded recovery, and the event counter decays too, so a single knock does not
     * leave the cylinder permanently retarded. */
    const float recovery = rc_absf(rc_cal_get_knock_knock_recovery_deg_per_s(cal));
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        state->retard_deg[cyl] = rc_maxf(state->retard_deg[cyl] - (recovery * dt_s), 0.0f);
        if (state->retard_deg[cyl] <= 0.0f) {
            state->event_count[cyl] = 0u;
        }
    }
}

void rc_knock_publish(const rc_knock_state_t *state, const rc_cal_t *cal, rc_knock_output_t *out)
{
    if (state == NULL || cal == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const bool available = (rc_cal_get_knock_knock_enabled(cal) != 0u)
        && (rc_cal_get_strategy_knock_sensor_count(cal) != 0u);
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        out->retard_deg[cyl] = available ? state->retard_deg[cyl] : 0.0f;
        out->noise_floor[cyl] = state->noise_floor[cyl];
        out->active[cyl] = available && (state->retard_deg[cyl] > 0.0f);
    }
}
