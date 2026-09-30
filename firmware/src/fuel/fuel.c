/* RACCOON V2 - fuel pipeline. See fuel.h for the hard rules F1..F5. */
#include "rc/fuel.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"

/* Ideal-gas constant for dry air, J/(kg*K).  A named constant with a stated source,
 * not a magic number (spec section 102). */
#define RC_R_AIR 287.058f
/* Lambda to air/fuel ratio for the mass balance; both sides of the ratio are in the
 * same units so this is dimensionless. */
#define RC_GAS_CONSTANT 8.314462f

void rc_fuel_init(rc_fuel_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    for (uint8_t i = 0u; i < RC_MAX_CYLINDERS; ++i) {
        state->learner_trim[i] = 1.0f;
    }
}

float rc_fuel_ve_lookup(const void *cells, rc_cell_kind_t kind,
                        const void *rpm_axis, const void *map_axis,
                        float rpm, float map_kpa, float scale)
{
    /* A pure interpolation with no calibration dependency, so the tuner's offline tool can
     * call it and get the same number the ECU does - but it needs the axes, which is where
     * the cell indices actually come from. */
    return rc_table2d_interp_on(cells, kind, 16u, 16u, rpm_axis, map_axis, RC_CELL_U16,
                                rpm, map_kpa, scale);
}

float rc_fuel_target_lambda(const rc_cal_t *cal, const rc_engine_t *engine,
                            const rc_fuel_input_t *in)
{
    (void)engine;
    const float table = rc_table2d_interp_on(
        cal->fuel_target_lambda_table_cells, RC_CELL_F32, 8u, 8u,
        cal->fuel_target_lambda_table_rpm_axis_cells,
        cal->fuel_target_lambda_table_load_pct_axis_cells, RC_CELL_U16, in->rpm, in->load,
        1.0f);

    /* Cold enrichment is a *richening* of the target, bounded by the sensor's own
     * plausibility band, so a failed ECT cannot command an absurd mixture. */
    const float ect_enrich_c = rc_cal_get_fuel_target_lambda_cold_ect_c(cal);
    float cold = 1.0f;
    if (in->ect_c < ect_enrich_c) {
        const float span = rc_maxf(ect_enrich_c - in->ect_c, 1.0f);
        const float gain = rc_cal_get_fuel_target_lambda_cold_gain(cal);
        cold = 1.0f - rc_clampf(gain * (span / 100.0f), 0.0f, 0.5f);
    }

    /* Altitude: less air mass available, so a slightly richer target keeps the mixture. */
    const float baro_gain = rc_clampf(rc_cal_get_fuel_target_lambda_baro_gain(cal), 0.0f, 2.0f);
    const float baro = 1.0f + ((100.0f - in->baro_kpa) / 100.0f) * (baro_gain - 1.0f);

    const float bias = rc_clampf(rc_cal_get_fuel_target_lambda_bias(cal),
                                 rc_cal_get_lambda_lambda_target_bias_min(cal),
                                 rc_cal_get_lambda_lambda_target_bias_max(cal));

    return rc_clampf((table * cold * baro) + bias, 0.40f, 1.60f);
}

float rc_fuel_pulse_ms(const rc_cal_t *cal, float fuel_mass_g, float battery_v,
                       float fuel_pressure_kpa)
{
    if (!(fuel_mass_g > 0.0f)) {
        return 0.0f;
    }
    /* Injector flow is specified volumetrically at a reference pressure, so the mass
     * flow is derived once and everything else is a correction on the *width*. */
    const float flow_cc_per_s = rc_cal_get_fuel_injector_flow_cc_per_s(cal);
    const float reference_kpa = rc_cal_get_fuel_injector_reference_pressure_kpa(cal);
    const float fuel_density = rc_cal_get_engine_fuel_density_gasoline(cal);
    if (!(flow_cc_per_s > 0.0f) || !(reference_kpa > 0.0f) || !(fuel_density > 0.0f)) {
        return 0.0f;
    }
    /* g/s at the reference pressure. */
    const float mass_flow_g_per_s = (flow_cc_per_s * fuel_density) / 1000.0f;
    const float volumetric_scale = rc_safe_div(reference_kpa, fuel_pressure_kpa, 1.0f);
    const float effective_flow = mass_flow_g_per_s * volumetric_scale;
    if (!(effective_flow > 0.0f)) {
        return 0.0f;
    }

    const float base_ms = (fuel_mass_g / effective_flow) * 1000.0f;

    /* Battery compensation widens the pulse as the driver loses drive voltage. */
    const float nominal_v = 14.0f;
    const float battery_gain = 1.0f + ((nominal_v - battery_v)
                                        * rc_cal_get_fuel_injector_battery_comp_pct_per_v(cal) / 100.0f);

    float pulse = base_ms * battery_gain;

    /* Dead time: the electrical delay before any fuel flows. */
    pulse += rc_cal_get_fuel_injector_dead_time_ms(cal)
        + ((nominal_v - battery_v) * rc_cal_get_fuel_injector_dead_time_battery_slope(cal));

    /* F2: small-pulse compensation only above the minimum pulse.  Below the minimum the
     * injector has not opened at all, and the pulse is a pure dead-time correction. */
    const float min_pulse = rc_cal_get_fuel_injector_min_pulse_ms(cal);
    const float compensation = rc_cal_get_fuel_injector_small_pulse_comp_pct(cal) / 100.0f;
    if (pulse < min_pulse * 2.0f) {
        pulse += pulse * compensation;
    }

    /* F3: bounds. */
    const float max_pulse = rc_cal_get_fuel_injector_max_pulse_ms(cal);
    return rc_clampf(pulse, min_pulse, max_pulse);
}

static void update_learner(rc_fuel_state_t *state, const rc_cal_t *cal,
                           const rc_fuel_input_t *in, uint8_t cylinder, float measured_ratio)
{
    /* The learner only ever moves toward the measured ratio, one bounded step at a time,
     * and it refuses to move at all while the conditions are not steady. */
    const bool eligible = (in->lambda_valid != 0u) && (in->crank_only == false)
        && (in->ect_c > 40.0f) && (measured_ratio > 0.5f) && (measured_ratio < 2.0f);
    if (!eligible) {
        return;
    }
    if (in->dt_s <= 0.0f) {
        return;
    }
    /* A step limited by a rate, so the trim can never jump between cycles.  The rate comes
     * from the calibration rather than a constant: how fast a tuner is willing to let the
     * ECU rewrite its own fuel map is exactly the kind of decision that belongs in the tune,
     * not in the firmware. */
    const float rate_pct = rc_cal_get_fuel_learner_rate_limit_pct_per_s(cal);
    const float rate = (rate_pct > 0.0f) ? (rate_pct / 100.0f) : 0.0f;
    const float step = rc_clampf((measured_ratio - state->learner_trim[cylinder]),
                                 -rate * in->dt_s, rate * in->dt_s);
    /* The trim is bounded per cylinder: at the bound the learner freezes rather than
     * continuing to walk, which is what turns a bad day into a visible fault instead of
     * a silently corrupted table. */
    const float limit = 1.0f + (rc_cal_get_fuel_learner_trim_max_pct(cal) / 100.0f);
    const float before = state->learner_trim[cylinder];
    state->learner_trim[cylinder] = rc_clampf(before + step, rc_safe_div(1.0f, limit, 1.0f), limit);
    if (state->learner_trim[cylinder] == before && step != 0.0f) {
        state->learner_saturated = true;
    }
    state->learner_samples++;
}

void rc_fuel_update(rc_fuel_state_t *state, const rc_cal_t *cal, const rc_engine_t *engine,
                    const rc_fuel_input_t *in, rc_cut_reason_t cut_active,
                    float torque_allowance, float safety_factor, rc_fuel_output_t *out)
{
    if (state == NULL || cal == NULL || engine == NULL || in == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0u; i < RC_MAX_CYLINDERS; ++i) {
        out->trim[i] = state->learner_trim[i];
    }

    /* ------------------------------------------------------ target lambda */
    out->target_lambda = rc_fuel_target_lambda(cal, engine, in);

    /* ---------------------------------------------------------- air mass */
    /* The charge is drawn from the MANIFOLD, so the pressure in the ideal gas law is the
     * absolute manifold pressure - not the rise above barometric.
     *
     * Using (MAP - baro) is a pressure-drop formulation and it is wrong at exactly the
     * point where it matters most.  A throttled engine at idle runs 30-40 kPa absolute,
     * well BELOW atmospheric, so the difference clamps to zero, the air mass becomes zero,
     * no fuel is commanded, and the engine cannot idle at all.  That is not a tuning
     * subtlety: it is a closed-throttle engine that dies.
     *
     * Volumetric efficiency already carries the throttling, and the trapped charge is the
     * manifold pressure, which is also the VE table's column axis - so the column is
     * looked up on map_abs, on the real breakpoints. */
    const float map_abs = rc_maxf(in->map_kpa, 1.0f);
    const float map_pa = map_abs * 1000.0f;
    const float iat_k = rc_maxf(in->iat_c + 273.15f, 150.0f);
    const float volume_m3 = (engine->swept_cc * 1.0e-6f)
        * rc_clampf(rc_table2d_interp_on(cal->fuel_ve_table_cells, RC_CELL_U8, 16u, 16u,
                                         cal->fuel_ve_table_rpm_axis_cells,
                                         cal->fuel_ve_table_map_kpa_axis_cells,
                                         RC_CELL_U16, in->rpm, map_abs,
                                         1.0f) / 100.0f, 0.05f, 1.50f);
    /* Real-gas correction, bounded: it is a small correction at the pressures a motorcycle
     * sees, and the ideal term carries the answer. */
    const float real_gas = 1.0f + (0.00035f * map_abs);
    out->air_mass_g = rc_safe_div((map_pa * volume_m3 * real_gas * 1000.0f) / (RC_R_AIR * iat_k),
                                  1.0f, 0.0f);

    /* --------------------------------------------------------- fuel mass */
    /* AFR = lambda * stoich for the blended fuel, and air_mass = fuel_mass * AFR, so the
     * fuel mass follows directly. The stoich AFR is itself a blend of gasoline and
     * ethanol, not a constant, which is the whole point of the flex-fuel support. */
    const float ethanol = rc_clampf(rc_cal_get_engine_fuel_ethanol_fraction(cal), 0.0f, 1.0f);
    const float stoich = rc_cal_get_engine_fuel_stoich_afr_gasoline(cal)
        + ((rc_cal_get_engine_fuel_stoich_afr_ethanol(cal)
            - rc_cal_get_engine_fuel_stoich_afr_gasoline(cal)) * ethanol);
    const float afr = out->target_lambda * rc_maxf(stoich, 1.0f);
    out->fuel_mass_g = rc_safe_div(out->air_mass_g, afr, 0.0f);
    out->base_pulse_ms = rc_fuel_pulse_ms(cal, out->fuel_mass_g, in->battery_v,
                                          in->fuel_pressure_kpa);

    /* --------------------------------------------------------- transient */
    /* F5: exactly one accumulator, with a maximum and a decay. */
    const float dt = in->dt_s;
    const float tps_rate = rc_safe_div(in->tps_pct - state->previous_tps, dt, 0.0f);
    const float map_rate = rc_safe_div(in->map_kpa - state->previous_map, dt, 0.0f);
    float demand = 0.0f;
    if (tps_rate > rc_cal_get_fuel_transient_tps_threshold(cal)) {
        demand += (tps_rate - rc_cal_get_fuel_transient_tps_threshold(cal))
            * rc_cal_get_fuel_transient_tps_gain(cal);
    }
    if (map_rate > rc_cal_get_fuel_transient_map_threshold(cal)) {
        demand += (map_rate - rc_cal_get_fuel_transient_map_threshold(cal))
            * rc_cal_get_fuel_transient_map_gain(cal);
    }
    const float transient_max = rc_cal_get_fuel_transient_tps_max_pct(cal);
    const float tau = rc_cal_get_fuel_transient_tps_tau_ms(cal) * 1.0e-3f;
    state->enrichment_pct = rc_decay(state->enrichment_pct, rc_clampf(demand, 0.0f, transient_max),
                                     dt, tau);

    /* Wall wetting: the film left on the port from the previous cycle, decaying.  It
     * reduces the additional pulse needed, which is why the two terms have opposite
     * signs and must not be merged into one hidden multiplier. */
    const float wall_tau = rc_cal_get_fuel_wall_wet_tau_ms(cal) * 1.0e-3f;
    const float wall_max = rc_cal_get_fuel_wall_wet_max_pct(cal);
    state->wall_wet_pct = rc_decay(state->wall_wet_pct, 0.0f, dt, wall_tau);
    state->wall_wet_pct = rc_clampf(state->wall_wet_pct, 0.0f, wall_max);

    const float net_enrichment = rc_clampf(state->enrichment_pct - state->wall_wet_pct,
                                           -wall_max, transient_max);
    out->transient_pulse_ms = out->base_pulse_ms * (net_enrichment / 100.0f);
    state->previous_tps = in->tps_pct;
    state->previous_map = in->map_kpa;

    /* ------------------------------------------------------- closed loop */
    out->closed_loop_factor = 1.0f;
    out->closed_loop_active = false;
    if ((in->lambda_valid != 0u) && (in->crank_only == false) && (in->rpm > 800.0f)) {
        /* A bounded proportional correction with a hard clamp.  The gain is calibrated
         * and its stability margin is measured in SIL (UNK-FW-010); nothing here may
         * grow without bound. */
        const float gain = rc_cal_get_lambda_lambda_closed_loop_gain(cal);
        /* A HIGHER lambda is a LEANER mixture, so correcting a lean reading means ADDING
         * fuel.  The correction therefore rises with the ratio.
         *
         * The opposite sign - 1 - (ratio - 1) - looks plausible and is exactly backwards:
         * it makes a lean engine richer and a rich engine leaner, so a closed-loop engine
         * is driven straight away from its target, the correction saturates at the wrong
         * end, and the ECU spends its time fighting the mixture it is trying to hold.  A
         * unit test that only checked "the factor is not 1" would pass this happily. */
        const float ratio = rc_safe_div(in->lambda, out->target_lambda, 1.0f);
        out->closed_loop_factor = rc_clampf(1.0f + ((ratio - 1.0f) * gain), 0.60f, 1.60f);
        out->closed_loop_active = true;
    }

    /* ------------------------------------------------------------- learner */
    if (rc_cal_get_fuel_learner_enabled(cal) != 0u) {
        const float ratio = rc_safe_div(in->lambda, out->target_lambda, 1.0f);
        for (uint8_t cyl = 0u; cyl < engine->cylinder_count; ++cyl) {
            update_learner(state, cal, in, cyl, ratio);
        }
    }
    out->cylinder_trim = state->learner_trim[0];

    /* --------------------------------------------------- cut and authority */
    /* F1: the cut arbiter runs last and is not negotiable. */
    out->fuel_cut = (cut_active != RC_CUT_NONE);
    out->cut_reason = (uint8_t)cut_active;
    if (state->decel_latched && cut_active == RC_CUT_NONE) {
        out->fuel_cut = true;
        out->cut_reason = (uint8_t)RC_CUT_DECELERATION;
    }

    /* Duty ceiling: the only place the pulse is finally limited by available time. */
    const float ms_per_cycle = rc_safe_div(120000.0f, rc_maxf(in->rpm, 1.0f), 1000.0f);
    const float duty = rc_safe_div(out->base_pulse_ms + out->transient_pulse_ms,
                                   ms_per_cycle, 1.0f) * 100.0f;
    out->duty_percent = rc_clampf(duty, 0.0f, 100.0f);

    if (out->fuel_cut) {
        /* A cut is zero fuel, unconditionally.  Not the minimum pulse, not the dead
         * time, not anything else: zero. */
        out->base_pulse_ms = 0.0f;
        out->transient_pulse_ms = 0.0f;
        out->duty_percent = 0.0f;
        state->enrichment_pct = 0.0f;
        state->wall_wet_pct = 0.0f;
        return;
    }

    /* Zero is a cut, not a small demand.
     *
     *  Two different things can make the result zero, and both are deliberate instructions
     *  rather than a very small number: there may be no demand at all (a vacuum), or an
     *  authority may have withdrawn all of it (a torque or safety cut).  Both must command
     *  exactly zero.  Raising either to the minimum pulse would inject fuel into an engine
     *  that is drawing none, and - worse - would silently defeat every torque and safety
     *  cut in the system, because a cut that still injects is not a cut.
     *
     *  The minimum pulse is therefore a floor for a POSITIVE demand that has merely been
     *  reduced, never for an instruction to deliver nothing. */
    const float total_demand = out->base_pulse_ms + out->transient_pulse_ms;
    const bool any_authority_withdrawn = (out->closed_loop_factor <= 0.0f)
        || (out->cylinder_trim <= 0.0f)
        || (rc_clampf(torque_allowance, 0.0f, 1.0f) <= 0.0f)
        || (rc_clampf(safety_factor, 0.0f, 1.0f) <= 0.0f);
    const bool suppressed = any_authority_withdrawn || !(total_demand > 0.0f);

    /* Closed loop, trim and authority, applied as bounded multiplicative terms. */
    float pulse = total_demand * out->closed_loop_factor;
    pulse *= rc_clampf(out->cylinder_trim, 0.5f, 1.5f);
    pulse *= rc_clampf(torque_allowance, 0.0f, 1.0f);
    pulse *= rc_clampf(safety_factor, 0.0f, 1.0f);

    /* The final clamp's lower bound is the MINIMUM PULSE, not zero.  Closed loop, trim,
     * torque allowance and safety factor are all multiplicative, so a stack of reductions
     * can drive the commanded pulse below the injector's dead time.  A pulse shorter than
     * dead time does not deliver a little less fuel, it delivers NO fuel while the ECU
     * believes it is fuelling the engine - so the result is a lean stumble that no
     * downstream clamp can explain.  Fuel cut is the only path that may command zero, and
     * it returns above. */
    const float lower = suppressed ? 0.0f : rc_cal_get_fuel_injector_min_pulse_ms(cal);
    out->base_pulse_ms = rc_clampf(pulse, lower,
                                   rc_cal_get_fuel_injector_max_pulse_ms(cal));
}

const char *rc_fuel_cut_name(uint8_t reason)
{
    switch (reason) {
        case RC_CUT_NONE: return "none";
        case RC_CUT_DECELERATION: return "deceleration";
        case RC_CUT_REV_LIMIT: return "rev limiter";
        case RC_CUT_LAUNCH: return "launch";
        case RC_CUT_SHIFT: return "shift";
        case RC_CUT_TRACTION: return "traction";
        case RC_CUT_SAFETY: return "safety";
        case RC_CUT_SENSOR_FAULT: return "sensor fault";
        case RC_CUT_KILL_SWITCH: return "kill switch";
        case RC_CUT_PRESSURE: return "fuel pressure";
        default: return "invalid";
    }
}
