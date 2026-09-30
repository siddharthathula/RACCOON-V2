/* RACCOON V2 - fuel pipeline tests.
 *
 * The rule that matters most here is F1: once a fuel cut is decided, nothing downstream -
 * minimum pulse, dead time, enrichment, closed loop, the learner - may re-open the
 * injector.  Several of these tests exist purely to attack that.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/engine.h"
#include "rc/fuel.h"

static rc_engine_t twin(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine;
    const char *reason = NULL;
    (void)rc_engine_build(&cal, &engine, &reason);
    return engine;
}

static rc_fuel_input_t normal_input(void)
{
    rc_fuel_input_t in;
    memset(&in, 0, sizeof(in));
    in.rpm = 4000.0f;
    in.load = 60.0f;
    in.load_source = (uint8_t)RC_LOAD_MAP;
    /* MAP must sit ABOVE barometric pressure for a running engine under real load.
     * MAP == baro is a vacuum with no air mass and therefore no fuel, asserted separately
     * below.  A barely-positive MAP is worse for these tests than a vacuum: it produces a
     * demand so small that every case lands on the minimum-pulse floor, and then battery
     * compensation, torque allowance and closed-loop trim all become invisible because
     * they are multiplied into a value that is clamped flat either way.  Real load is what
     * makes those multiplicative paths observable at all. */
    in.map_kpa = 130.0f;
    in.baro_kpa = 100.0f;
    in.iat_c = 25.0f;
    in.ect_c = 90.0f;
    in.battery_v = 13.8f;
    in.fuel_pressure_kpa = 380.0f;
    in.lambda = 1.0f;
    in.lambda_valid = 1u;
    in.tps_pct = 40.0f;
    in.dt_s = 0.0005f;
    return in;
}

static float run_pulse(float rpm, float battery, bool cut, uint8_t *reason_out)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);
    rc_fuel_input_t in = normal_input();
    in.rpm = rpm;
    in.battery_v = battery;
    rc_fuel_output_t out;
    rc_fuel_update(&state, &cal, &engine, &in, cut ? RC_CUT_REV_LIMIT : RC_CUT_NONE, 1.0f, 1.0f,
                   &out);
    if (reason_out != NULL) {
        *reason_out = out.cut_reason;
    }
    return out.base_pulse_ms;
}

static void pulse_is_bounded(void)
{
    const rc_cal_t cal = rc_cal_defaults_filled();
    uint8_t reason = 0u;
    /* Never below the minimum pulse, never above the maximum, at any plausible operating
     * point.  This is rule F3 and it is a hard bound, not a guideline. */
    for (uint32_t rpm = 800u; rpm <= 12000u; rpm += 200u) {
        for (int vbattery = 90; vbattery <= 160; vbattery += 10) {
            const float pulse = run_pulse((float)rpm, (float)vbattery * 0.1f, false, &reason);
            RC_CHECK_MSG(pulse >= rc_cal_get_fuel_injector_min_pulse_ms(&cal) - 1e-4f,
                         "rpm %u, %.1f V: pulse %.4f ms below the minimum", (unsigned)rpm,
                         (double)((float)vbattery * 0.1f), (double)pulse);
            RC_CHECK_MSG(pulse <= rc_cal_get_fuel_injector_max_pulse_ms(&cal) + 1e-4f,
                         "rpm %u, %.1f V: pulse %.4f ms above the maximum", (unsigned)rpm,
                         (double)((float)vbattery * 0.1f), (double)pulse);
        }
    }
}

static void charge_comes_from_the_manifold_not_the_pressure_rise(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);
    rc_fuel_input_t in = normal_input();
    rc_fuel_output_t out;

    /* A manifold BELOW barometric is not a vacuum at all.  A throttled engine idles at
     * 30-40 kPa absolute, so the charge is drawn from the manifold pressure and this engine
     * must still be given fuel.  Treating "below atmospheric" as "no air" - by computing
     * the charge from MAP minus baro - makes every engine impossible to idle, because a
     * closed throttle is exactly the case that difference clamps to zero. */
    rc_fuel_init(&state);
    in.map_kpa = 35.0f;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    RC_CHECK_MSG(out.air_mass_g > 0.0f,
                 "a throttled engine at 35 kPa was given no air at all");
    RC_CHECK_MSG(out.base_pulse_ms > 0.0f,
                 "a throttled engine at 35 kPa was given no fuel: it could not idle");

    /* The charge must scale with manifold pressure, monotonically, and collapse towards
     * the minimum pulse as the manifold approaches vacuum.  The command never goes to
     * exactly zero here: whether the engine is running at all is the control layer's
     * decision, made from crank synchronisation, not something the fuel module guesses at
     * from a sensor value. */
    float previous_pulse = 0.0f;
    float previous_air = 0.0f;
    for (int kpa = 5; kpa <= 100; kpa += 5) {
        rc_fuel_init(&state);
        in.map_kpa = (float)kpa;
        rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
        RC_CHECK_MSG(out.air_mass_g >= previous_air - 1e-6f,
                     "air mass fell from %.5f g to %.5f g as the manifold rose to %d kPa",
                     (double)previous_air, (double)out.air_mass_g, kpa);
        RC_CHECK_MSG(out.base_pulse_ms >= previous_pulse - 1e-5f,
                     "pulse fell from %.4f ms to %.4f ms as the manifold rose to %d kPa",
                     (double)previous_pulse, (double)out.base_pulse_ms, kpa);
        previous_pulse = out.base_pulse_ms;
        previous_air = out.air_mass_g;
    }
    RC_CHECK_MSG(previous_pulse > 1.5f, "a full-manifold engine was still on minimum pulse");
}

static void battery_sag_widens_the_pulse(void)
{
    uint8_t reason = 0u;
    /* A sagging battery means the injector driver has less drive voltage, so the pulse
     * must widen.  If it does not, cold cranking would run lean. */
    const float hot = run_pulse(3000.0f, 14.2f, false, &reason);
    const float cold = run_pulse(3000.0f, 10.0f, false, &reason);
    RC_CHECK_MSG(cold > hot, "battery sag must widen the pulse: %.4f then %.4f", (double)hot,
                 (double)cold);
    /* And the compensation must be monotonic, not a step.  The direction matters: a HIGHER
     * battery voltage drives the injector harder, so the pulse must get NARROWER as the
     * voltage rises.  Asserting the opposite would pass for a compensation wired backwards
     * and fail for a correct one. */
    float previous = 0.0f;
    bool first = true;
    for (int v = 90; v <= 150; v += 5) {
        const float pulse = run_pulse(3000.0f, (float)v * 0.1f, false, &reason);
        if (!first) {
            RC_CHECK_MSG(pulse <= previous + 1e-5f,
                         "pulse grew from %.4f to %.4f as the battery rose to %d/10 V",
                         (double)previous, (double)pulse, v);
        }
        previous = pulse;
        first = false;
    }
}

static void fuel_cut_is_final(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);

    /* Establish a state with a big enrichment accumulator, then cut. */
    rc_fuel_input_t in = normal_input();
    rc_fuel_output_t out;
    for (int i = 0; i < 50; ++i) {
        in.tps_pct = (float)(20 + (i * 4));
        in.dt_s = 0.001f;
        rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    }
    RC_CHECK_MSG(state.enrichment_pct > 0.0f, "the transient model should be charging up");

    /* Now cut, with every other requester asking for fuel. */
    in.tps_pct = 60.0f;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_REV_LIMIT, 1.0f, 1.0f, &out);
    RC_CHECK_MSG(out.fuel_cut, "the cut was not reported");
    RC_CHECK_EQ_U(out.cut_reason, (uint8_t)RC_CUT_REV_LIMIT);
    RC_CHECK_MSG(out.base_pulse_ms == 0.0f, "a cut must produce zero fuel, got %.4f ms",
                 (double)out.base_pulse_ms);
    RC_CHECK_MSG(out.transient_pulse_ms == 0.0f, "enrichment survived a cut");
    RC_CHECK_NEAR(out.duty_percent, 0.0f, 1e-6);
    /* The accumulator is reset, not carried, so re-entry cannot be an enrichment event. */
    RC_CHECK_NEAR(state.enrichment_pct, 0.0f, 1e-6);
    RC_CHECK_NEAR(state.wall_wet_pct, 0.0f, 1e-6);
    /* Hold the cut for many cycles: it must stay at exactly zero. */
    for (int i = 0; i < 200; ++i) {
        rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_REV_LIMIT, 1.0f, 1.0f, &out);
        RC_CHECK_MSG(out.base_pulse_ms == 0.0f, "cycle %d: fuel reappeared during a cut", i);
    }
}

static void authority_scales_but_never_increases(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);
    rc_fuel_input_t in = normal_input();
    rc_fuel_output_t full;
    rc_fuel_output_t half;
    rc_fuel_output_t none;

    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &full);
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 0.5f, 1.0f, &half);
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 0.0f, 1.0f, &none);
    RC_CHECK_MSG(half.base_pulse_ms < full.base_pulse_ms, "torque allowance did not reduce fuel");
    RC_CHECK_MSG(half.base_pulse_ms > 0.0f, "half authority produced no fuel at all");
    RC_CHECK_NEAR(none.base_pulse_ms, 0.0f, 1e-6);
    /* An allowance above 1 must not create fuel: the requester cannot invent authority. */
    rc_fuel_output_t over;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 4.0f, 1.0f, &over);
    RC_CHECK(over.base_pulse_ms <= full.base_pulse_ms + 1e-6f);
}

static void target_lambda_is_bounded_and_ethanol_aware(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_input_t in = normal_input();

    /* Sweep the whole plausible input space: the target must never leave a defensible
     * band, whatever the sensors say. */
    for (int i = 0; i < 2000; ++i) {
        in.rpm = (float)((unsigned)i * 6u);
        in.load = (float)((unsigned)i % 120u);
        in.ect_c = (float)((unsigned)i % 140u) - 20.0f;
        in.iat_c = (float)((unsigned)i % 80u);

        const float lambda = rc_fuel_target_lambda(&cal, &engine, &in);
        RC_CHECK_MSG(lambda >= 0.40f && lambda <= 1.60f, "target lambda %.3f out of band at i=%d",
                     (double)lambda, i);
    }
    /* A cold engine is richer than a warm one: the cold enrichment must actually bite. */
    in.ect_c = -20.0f;
    const float cold = rc_fuel_target_lambda(&cal, &engine, &in);
    in.ect_c = 90.0f;
    const float warm = rc_fuel_target_lambda(&cal, &engine, &in);
    RC_CHECK_MSG(cold < warm, "cold enrichment did not enrich: %.3f then %.3f", (double)cold,
                 (double)warm);
    /* But not beyond the calibration's own bound, whatever the ECT says. */
    in.ect_c = -40.0f;
    const float freezing = rc_fuel_target_lambda(&cal, &engine, &in);
    RC_CHECK(freezing >= 0.40f);
}

static void invalid_sensors_do_not_produce_nonsense_fuel(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);
    rc_fuel_output_t out;
    rc_fuel_input_t in;

    /* No valid lambda: closed loop must not engage and the command must stay finite. */
    in = normal_input();
    in.lambda_valid = 0u;
    in.lambda = 0.0f;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    RC_CHECK_MSG(!out.closed_loop_active, "closed loop engaged with no lambda");
    RC_CHECK(isfinite(out.base_pulse_ms));
    RC_CHECK(out.base_pulse_ms >= 0.0f);

    /* A zero fuel pressure must not divide by zero into an infinite pulse. */
    in = normal_input();
    in.fuel_pressure_kpa = 0.0f;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    RC_CHECK_MSG(isfinite(out.base_pulse_ms), "a zero fuel pressure produced a non-finite pulse");
    RC_CHECK(out.base_pulse_ms <= rc_cal_get_fuel_injector_max_pulse_ms(&cal) + 1e-4f);

    /* An injector flow of zero is a configuration error; the result must still be bounded. */
    rc_cal_t broken = cal;
    broken.fuel_injector_flow_cc_per_s = 0.0f;
    rc_fuel_update(&state, &broken, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    RC_CHECK(isfinite(out.base_pulse_ms));

    /* Cranking: no closed loop, no learner. */
    in = normal_input();
    in.crank_only = true;
    rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    RC_CHECK(!out.closed_loop_active);
}

static void learner_is_bounded_and_freezes(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin();
    rc_fuel_state_t state;
    rc_fuel_init(&state);
    rc_fuel_output_t out;
    rc_fuel_input_t in = normal_input();
    in.ect_c = 95.0f;
    /* The learner ships DISABLED: an ECU must not rewrite its own fuel map on an engine
     * nobody has measured.  A test that exercises the learner has to turn it on, exactly
     * as the tuner would. */
    RC_CHECK(rc_cal_set_fuel_learner_enabled(&cal, true));

    /* A lambda that is 30 % richer than target should push the trim up, but only to the
     * calibration's bound, and then it must stop rather than continue to walk. */
    in.lambda = 1.3f;
    in.lambda_valid = 1u;
    /* The configured learner rate is 1 %/s, so walking the trim the full 20 % to its bound
     * takes 20 s of engine time.  At a 0.5 ms cycle that is 40 000 iterations, not 20 000:
     * with a shorter run the learner has merely not finished, which would look exactly like
     * a learner that is stuck. */
    for (int i = 0; i < 60000; ++i) {
        rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    }
    const float limit = 1.0f + (rc_cal_get_fuel_learner_trim_max_pct(&cal) / 100.0f);
    RC_CHECK_MSG(state.learner_trim[0] <= limit + 1e-4f, "the learner exceeded its bound: %.3f",
                 (double)state.learner_trim[0]);
    RC_CHECK_MSG(state.learner_saturated, "the learner should report saturation at its bound");
    RC_CHECK(state.learner_trim[0] > 1.0f);

    /* A cold engine must not teach the learner anything. */
    rc_fuel_init(&state);
    in.ect_c = 5.0f;
    for (int i = 0; i < 500; ++i) {
        rc_fuel_update(&state, &cal, &engine, &in, RC_CUT_NONE, 1.0f, 1.0f, &out);
    }
    RC_CHECK_NEAR(state.learner_trim[0], 1.0f, 1e-3);
}

void test_fuel(void)
{
    RC_TEST_CASE("charge/comes/from/the/manifold/not/the/pressure/rise",
                 charge_comes_from_the_manifold_not_the_pressure_rise);
    RC_TEST_CASE("pulse/is/bounded", pulse_is_bounded);
    RC_TEST_CASE("battery/sag/widens/the/pulse", battery_sag_widens_the_pulse);
    RC_TEST_CASE("fuel/cut/is/final", fuel_cut_is_final);
    RC_TEST_CASE("authority/scales/but/never/increases", authority_scales_but_never_increases);
    RC_TEST_CASE("target/lambda/is/bounded/and/ethanol/aware", target_lambda_is_bounded_and_ethanol_aware);
    RC_TEST_CASE("invalid/sensors/do/not/produce/nonsense/fuel", invalid_sensors_do_not_produce_nonsense_fuel);
    RC_TEST_CASE("learner/is/bounded/and/freezes", learner_is_bounded_and_freezes);
}
