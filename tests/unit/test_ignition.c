/* RACCOON V2 - ignition and knock tests. */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/engine.h"
#include "rc/cal_validate.h"
#include "rc/ignition.h"

static rc_engine_t twin(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine;
    const char *reason = NULL;
    (void)rc_engine_build(&cal, &engine, &reason);
    return engine;
}

static rc_ignition_input_t normal_input(void)
{
    rc_ignition_input_t in;
    memset(&in, 0, sizeof(in));
    in.rpm = 4000.0f;
    in.load = 60.0f;
    in.iat_c = 25.0f;
    in.ect_c = 90.0f;
    in.baro_kpa = 100.0f;
    in.battery_v = 13.8f;
    in.tps_pct = 50.0f;
    in.dt_s = 0.0005f;
    return in;
}

static void envelope_is_never_exceeded(void)
{
    /* Rule I2, and the single most important property in the ignition module: for ANY
     * combination of corrections and user trim, the final command lands inside the hard
     * envelope.  Swept rather than sampled, because the danger is a combination. */
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    const float env_min = rc_cal_get_strategy_ignition_hard_min_deg(&cal);
    const float env_max = rc_cal_get_strategy_ignition_hard_max_deg(&cal);
    const float trim_max = rc_cal_get_ignition_ignition_user_trim_max_deg(&cal);

    int limited = 0;
    for (int i = 0; i < 20000; ++i) {
        rc_ignition_input_t in = normal_input();
        in.rpm = (float)((unsigned)i % 12000u);
        in.load = (float)((unsigned)i % 120u);
        in.iat_c = (float)(i % 120) - 40.0f;
        in.ect_c = (float)(((unsigned)i * 7u) % 200u) - 40.0f;
        in.battery_v = (float)(60u + (((unsigned)i * 13u) % 180u)) * 0.1f;
        in.ethanol_fraction = (float)((unsigned)i % 101u) / 100.0f;
        in.baro_kpa = (float)(50u + ((unsigned)i % 60u));
        in.user_trim_deg = ((i % 2) == 0) ? trim_max : -trim_max;
        rc_ignition_output_t out;
        rc_ignition_update(&cal, &engine, NULL, &in, NULL, &out);
        for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
            RC_CHECK_MSG(out.final_deg[cyl] >= env_min - 1e-3f && out.final_deg[cyl] <= env_max + 1e-3f,
                         "case %d cyl %u: %.3f deg outside [%.1f,%.1f]", i, (unsigned)cyl,
                         (double)out.final_deg[cyl], (double)env_min, (double)env_max);
        }
        if (out.envelope_limited) {
            limited++;
        }
    }
    /* The extremes genuinely do clamp, so the sweep is not passing vacuously. */
    RC_CHECK_MSG(limited > 0, "no case was limited, so the envelope test proves nothing");
}

static void corrections_are_bounded_and_directional(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_ignition_input_t warm = normal_input();
    rc_ignition_input_t cold = normal_input();
    cold.ect_c = -20.0f;
    /* A warm engine needs less advance than a cold one, and the amount is bounded. */
    RC_CHECK(rc_ignition_correction_deg(&cal, &warm) < rc_ignition_correction_deg(&cal, &cold));
    /* Even an absurd sensor value cannot produce an unbounded correction. */
    rc_ignition_input_t silly = normal_input();
    silly.ect_c = 500.0f;
    silly.iat_c = 500.0f;
    silly.ethanol_fraction = 5.0f;
    silly.user_trim_deg = 1.0e6f;
    const float corr = rc_ignition_correction_deg(&cal, &silly);
    RC_CHECK_MSG(fabs((double)corr) < 200.0, "an absurd input produced %.1f deg of correction",
                 (double)corr);
}

static void dwell_is_bounded_by_hardware(void)
{
    const rc_cal_t cal = rc_cal_defaults_filled();
    const float soft_min = rc_cal_get_ignition_dwell_min_ms(&cal);
    const float soft_max = rc_cal_get_ignition_dwell_max_ms(&cal);
    const float hardware = rc_cal_get_hardware_ignition_max_dwell_ms(&cal);
    /* Sweep the whole battery x rpm domain the table is indexed on. */
    for (int bv = 0; bv <= 200; bv += 5) {
        for (int rpm = 0; rpm <= 14000; rpm += 500) {
            const float dwell = rc_ignition_dwell_ms(&cal, (float)bv * 0.1f, (float)rpm);
            RC_CHECK_MSG(dwell >= soft_min - 1e-4f && dwell <= soft_max + 1e-4f,
                         "%.1f V, %d rpm: dwell %.3f ms outside [%.2f,%.2f]", (double)bv * 0.1, rpm,
                         (double)dwell, (double)soft_min, (double)soft_max);
            RC_CHECK_MSG(dwell <= hardware + 1e-4f, "dwell %.3f exceeded the hardware ceiling %.2f",
                         (double)dwell, (double)hardware);
        }
    }
    /* A software maximum above the hardware ceiling is a configuration error, and the
     * local clamp is the last line of defence against it. */
    rc_cal_t broken = cal;
    broken.ignition_dwell_max_ms = 12.0f;   /* above ignition_max_dwell_ms */
    broken.hardware_ignition_max_dwell_ms = 6.0f;
    const float dwell = rc_ignition_dwell_ms(&broken, 14.0f, 4000.0f);
    RC_CHECK(dwell <= 6.0f + 1e-4f);
    RC_CHECK(rc_cal_validate(&broken, NULL, 0u, NULL) == false);
}

static void cut_is_a_flag_not_an_angle(void)
{
    /* Rule I4: a per-cylinder cut must be a separate flag.  Expressing it as 0 degrees
     * would collide with a legitimate zero-advance command. */
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_ignition_input_t in = normal_input();
    const bool cut[RC_MAX_CYLINDERS] = {false, true, false, false};
    rc_ignition_output_t out;
    rc_ignition_update(&cal, &engine, NULL, &in, cut, &out);
    RC_CHECK_EQ_U(out.cut[0], 0u);
    RC_CHECK_EQ_U(out.cut[1], 1u);
    /* The commanded angle for the cut cylinder is still a real angle, not a sentinel. */
    RC_CHECK(out.final_deg[1] != 0.0f);
}

static void knock_needs_a_sensor(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_knock_state_t state;

    rc_ignition_init(&state);
    /* Knock enabled, no sensor fitted: the retard must stay at zero and the feature is
     * reported unavailable.  A product must never claim knock control it cannot do. */
    cal.knock_knock_enabled = 1u;
    cal.strategy_knock_sensor_count = 0u;
    for (int i = 0; i < 100; ++i) {
        rc_knock_update(&state, &cal, 0u, 10.0f, 1.0f);
    }
    RC_CHECK_NEAR(state.retard_deg[0], 0.0f, 1e-6);
    rc_knock_output_t out;
    rc_knock_publish(&state, &cal, &out);
    RC_CHECK_NEAR(out.retard_deg[0], 0.0f, 1e-6);
    RC_CHECK(!out.active[0]);
}

static void knock_retard_accumulates_and_recovers(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.knock_knock_enabled = 1u;
    cal.strategy_knock_sensor_count = 1u;
    cal.knock_knock_threshold = 1.0f;
    cal.knock_knock_noise_floor = 1.0f;
    rc_knock_state_t state;
    rc_ignition_init(&state);

    /* Events inside the window accumulate a retard, bounded by the calibration. */
    const float max_retard = rc_cal_get_knock_knock_max_total_retard_deg(&cal);
    for (int i = 0; i < 500; ++i) {
        rc_knock_update(&state, &cal, 0u, 20.0f, 1.0f);
    }
    RC_CHECK(state.retard_deg[0] > 0.0f);
    RC_CHECK_MSG(state.retard_deg[0] <= rc_absf(max_retard) + 1e-4f,
                 "knock retard %.2f exceeded the maximum %.2f", (double)state.retard_deg[0],
                 (double)max_retard);
    const float accumulated = state.retard_deg[0];

    /* An event outside the window is not combustion knock and must not retard. */
    rc_knock_update(&state, &cal, 0u, 20.0f, 0.0f);
    RC_CHECK_NEAR(state.retard_deg[0], accumulated, 1e-4);

    /* A value under the threshold is not knock. */
    rc_knock_update(&state, &cal, 0u, 0.1f, 1.0f);
    RC_CHECK_NEAR(state.retard_deg[0], accumulated, 1e-4);

    /* Rule I5: recovery is bounded and gradual, never a snap back to full advance. */
    const float recovery = rc_absf(rc_cal_get_knock_knock_recovery_deg_per_s(&cal));

    rc_knock_decay(&state, &cal, 0.01f);
    const float after = state.retard_deg[0];
    RC_CHECK_MSG(after < accumulated, "the retard did not recover");
    RC_CHECK_MSG(accumulated - after <= (recovery * 0.01f) + 1e-4f,
                 "recovery exceeded the calibrated rate");
}

void test_ignition(void)
{
    RC_TEST_CASE("envelope/is/never/exceeded", envelope_is_never_exceeded);
    RC_TEST_CASE("corrections/are/bounded/and/directional", corrections_are_bounded_and_directional);
    RC_TEST_CASE("dwell/is/bounded/by/hardware", dwell_is_bounded_by_hardware);
    RC_TEST_CASE("cut/is/a/flag/not/an/angle", cut_is_a_flag_not_an_angle);
    RC_TEST_CASE("knock/needs/a/sensor", knock_needs_a_sensor);
    RC_TEST_CASE("knock/retard/accumulates/and/recovers", knock_retard_accumulates_and_recovers);
}
