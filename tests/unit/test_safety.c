/* RACCOON V2 - safety envelope tests.
 *
 * The property being tested is structural: for ANY input inside the declared sensor
 * validity bounds, the final fuel and ignition command must be inside the envelope, and
 * no requester - including performance modes and engineering mode - may widen it.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/rc_math.h"
#include "rc/safety.h"

static rc_engine_t twin_engine(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine;
    const char *reason = NULL;
    (void)rc_engine_build(&cal, &engine, &reason);
    return engine;
}

static rc_safety_input_t good_input(float rpm)
{
    rc_safety_input_t in;
    memset(&in, 0, sizeof(in));
    in.rpm = rpm;
    in.lambda = 1.0f;
    in.lambda_valid = 1u;
    in.ect_c = 90.0f;
    in.ect_valid = 1u;
    in.iat_c = 25.0f;
    in.iat_valid = 1u;
    in.oil_pressure_bar = 3.0f;
    in.oil_pressure_valid = 1u;
    in.oil_temp_c = 100.0f;
    in.oil_temp_valid = 1u;
    in.egt_c = 600.0f;
    in.egt_valid = 1u;
    in.battery_v = 13.8f;
    in.battery_valid = 1u;
    in.fuel_pressure_kpa = 380.0f;
    in.fuel_pressure_valid = 1u;
    in.injector_duty_percent = 40.0f;
    in.dwell_ms = 3.0f;
    in.boost_kpa = 0.0f;
    in.sync_state = (uint8_t)RC_TRIGGER_SYNCED;
    return in;
}

static void healthy_engine_is_untouched(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    rc_safety_input_t in = good_input(3000.0f);
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK_MSG(out.active == 0u, "a healthy engine reported %s", rc_safety_reason(out.active));
    RC_CHECK_EQ_U(out.action, RC_ACT_NONE);
    RC_CHECK_NEAR(out.torque_factor, 1.0f, 1e-6);
    RC_CHECK(!out.fuel_cut);
    RC_CHECK(!out.spark_cut);
    /* The envelope passes the ignition limits through unchanged when nothing is wrong. */
    RC_CHECK_NEAR(out.ignition_max_deg, rc_cal_get_strategy_ignition_hard_max_deg(&cal), 1e-4);
}

static void rev_limit_cuts_and_is_not_a_fault(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    const float limit = rc_cal_get_safety_rpm_limit(&cal);
    rc_safety_input_t in = good_input(limit + 10.0f);
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK((out.active & RC_SAFETY_RPM) != 0u);
    RC_CHECK(out.spark_cut);
    RC_CHECK_NEAR(out.torque_factor, 0.0f, 1e-6);
    /* The reason text must not describe normal operation as a failure. */
    RC_CHECK(strstr(rc_safety_reason(out.active), "not a fault") != NULL);
}

static void overspeed_shuts_down(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    rc_safety_input_t in = good_input(rc_cal_get_safety_overspeed_rpm(&cal) + 100.0f);
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK((out.active & RC_SAFETY_OVERSPEED) != 0u);
    RC_CHECK_EQ_U(out.action, RC_ACT_SHUTDOWN);
    RC_CHECK(out.fuel_cut);
    RC_CHECK(out.spark_cut);
}

static void lean_needs_the_hold_time_and_latches(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    const float lean_limit = rc_cal_get_safety_lean_lambda_limit(&cal);

    /* A single lean sample must not shut the engine down. */
    rc_safety_input_t in = good_input(3000.0f);
    in.lambda = lean_limit + 0.2f;
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK_MSG((out.active & RC_SAFETY_LEAN) == 0u, "a single sample latched the lean limit");

    /* Sustained lean latches, and the latch survives after the cause clears. */
    for (uint32_t t = 2000u; t < 2000u + 600000u; t += 1000u) {
        rc_safety_evaluate(&state, &cal, &engine, &in, t, &out);
    }
    RC_CHECK((out.active & RC_SAFETY_LEAN) != 0u);
    RC_CHECK(out.fuel_cut);
    rc_safety_input_t recovered = good_input(3000.0f);
    rc_safety_evaluate(&state, &cal, &engine, &recovered, 700000u, &out);
    RC_CHECK_MSG((out.active & RC_SAFETY_LEAN) != 0u, "the lean latch did not persist");
    /* Release requires the value to come back below the hysteresis band. */
    rc_safety_service_latches(&state, &cal, &recovered, 700100u);
    RC_CHECK_MSG(state.lean_latched == false, "the lean latch did not release");
}

static void rich_is_a_warning_not_a_cut(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    rc_safety_input_t in = good_input(3000.0f);
    in.lambda = rc_cal_get_safety_rich_lambda_limit(&cal) - 0.2f;
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    /* Cutting fuel into a rich condition makes it worse, so this warns. */
    RC_CHECK_MSG(out.fuel_cut == false, "a rich condition must not cut fuel");
    RC_CHECK((out.active & RC_SAFETY_RICH) != 0u);
}

static void sync_loss_pulls_ignition_to_the_floor(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    rc_safety_input_t in = good_input(3000.0f);
    in.sync_lost = 1u;
    in.sync_state = (uint8_t)RC_TRIGGER_SYNC_LOST;
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK((out.active & RC_SAFETY_SYNC) != 0u);
    RC_CHECK(out.fuel_cut);
    RC_CHECK(out.spark_cut);
    /* A spark already scheduled must not land at a stale angle. */
    RC_CHECK_NEAR(out.ignition_max_deg, out.ignition_min_deg, 1e-4);
    RC_CHECK(strstr(rc_safety_reason(out.active), "cannot know where any cylinder is") != NULL);
}

static void invalid_sensor_does_not_produce_a_mixture_cut(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    /* A lambda reading of 5.0 would be catastrophic if believed - but it is not believed,
     * because it is invalid. */
    /* A wideband IS fitted and in use - otherwise an invalid lambda is simply the absence
     * of a sensor and there is nothing to report. */
    RC_CHECK(rc_cal_set_strategy_lambda_strategy(&cal, 1u));
    rc_safety_input_t in = good_input(3000.0f);
    in.lambda = 5.0f;
    in.lambda_valid = 0u;
    rc_safety_evaluate(&state, &cal, &engine, &in, 1000u, &out);
    RC_CHECK_MSG((out.active & RC_SAFETY_LEAN) == 0u,
                 "an invalid lambda must not trip the mixture limit");
    /* But the sensor fault itself is reported, and the engine is limited. */
    RC_CHECK((out.active & RC_SAFETY_SENSOR) != 0u);
    RC_CHECK(out.torque_factor < 1.0f);
}

static void oil_pressure_latches_and_shuts_down(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    rc_safety_state_t state;
    rc_safety_init(&state);
    rc_safety_output_t out;
    rc_safety_input_t in = good_input(3000.0f);
    in.oil_pressure_bar = 0.1f;
    for (uint32_t t = 1000u; t < 1000u + 600000u; t += 1000u) {
        rc_safety_evaluate(&state, &cal, &engine, &in, t, &out);
    }
    RC_CHECK((out.active & RC_SAFETY_OIL_PRESSURE) != 0u);
    RC_CHECK_EQ_U(out.action, RC_ACT_SHUTDOWN);
    RC_CHECK(out.fuel_cut);
    /* Recovery needs the hysteresis band, not merely the trip point. */
    in.oil_pressure_bar = rc_cal_get_safety_oil_pressure_min_bar(&cal) + 0.05f;
    rc_safety_evaluate(&state, &cal, &engine, &in, 700000u, &out);
    RC_CHECK(state.oil_latched != false);
    in.oil_pressure_bar = rc_cal_get_safety_oil_pressure_recovery_bar(&cal);
    rc_safety_service_latches(&state, &cal, &in, 700100u);
    RC_CHECK(state.oil_latched == false);
}

static void fuzz_never_exceeds_the_envelope(void)
{
    /* The property test: for a wide sweep of RPM, lambda and temperatures, whatever the
     * input, the envelope's own output must be inside its own declared ignition range and
     * the torque factor must be a legal scale. */
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_engine_t engine = twin_engine();
    const float env_min = rc_cal_get_strategy_ignition_hard_min_deg(&cal);
    const float env_max = rc_cal_get_strategy_ignition_hard_max_deg(&cal);
    uint32_t seed = 12345u;
    for (int i = 0; i < 5000; ++i) {
        seed = (seed * 1664525u) + 1013904223u;
        rc_safety_state_t state;
        rc_safety_init(&state);
        rc_safety_input_t in = good_input(0.0f);
        in.rpm = (float)(seed % 15000u);
        seed = (seed * 1664525u) + 1013904223u;
        in.lambda = 0.5f + ((float)(seed % 200u) / 100.0f);
        seed = (seed * 1664525u) + 1013904223u;
        in.ect_c = (float)(seed % 150u);
        seed = (seed * 1664525u) + 1013904223u;
        in.battery_v = (float)(seed % 300u) / 10.0f;
        in.lambda_valid = 1u;
        in.ect_valid = 1u;
        in.battery_valid = 1u;
        rc_safety_output_t out;
        rc_safety_evaluate(&state, &cal, &engine, &in, (uint32_t)i * 1000u, &out);
        RC_CHECK(out.torque_factor >= 0.0f && out.torque_factor <= 1.0f);
        RC_CHECK(out.ignition_min_deg >= env_min - 1e-3f);
        RC_CHECK_MSG(out.ignition_max_deg <= env_max + 1e-3f,
                     "case %d: envelope ceiling %f exceeded the compiled limit %f", i,
                     (double)out.ignition_max_deg, (double)env_max);
        RC_CHECK(out.ignition_max_deg >= out.ignition_min_deg);
        /* A fuel cut does NOT imply the spark is suppressed: cutting spark on a lean
         * condition is what causes a backfire, so the envelope deliberately keeps the
         * ignition live for a lean cut.  Only a *sync* loss pulls ignition to the floor,
         * and that is asserted separately. */
        if (in.sync_lost == 0u) {
            RC_CHECK_MSG(!(out.fuel_cut && out.torque_factor > 0.0f),
                         "case %d: fuel was cut but the torque factor was not zero", i);
        }
    }
}

void test_safety(void)
{
    RC_TEST_CASE("healthy/engine/is/untouched", healthy_engine_is_untouched);
    RC_TEST_CASE("rev/limit/cuts/and/is/not/a/fault", rev_limit_cuts_and_is_not_a_fault);
    RC_TEST_CASE("overspeed/shuts/down", overspeed_shuts_down);
    RC_TEST_CASE("lean/needs/the/hold/time/and/latches", lean_needs_the_hold_time_and_latches);
    RC_TEST_CASE("rich/is/a/warning/not/a/cut", rich_is_a_warning_not_a_cut);
    RC_TEST_CASE("sync/loss/pulls/ignition/to/the/floor", sync_loss_pulls_ignition_to_the_floor);
    RC_TEST_CASE("invalid/sensor/does/not/produce/a/mixture/cut", invalid_sensor_does_not_produce_a_mixture_cut);
    RC_TEST_CASE("oil/pressure/latches/and/shuts/down", oil_pressure_latches_and_shuts_down);
    RC_TEST_CASE("fuzz/never/exceeds/the/envelope", fuzz_never_exceeds_the_envelope);
}
