/* RACCOON V2 - performance feature tests.
 *
 * Launch, quickshift, traction control, pit limiter, engine braking and wheelie
 * mitigation all live behind one rule: they are *requesters*.  They may only ever reduce
 * the authority the torque manager grants, they may never raise it, and none of them may
 * touch the final fuel or ignition command.  A performance feature that could add torque
 * is a safety defect wearing a feature flag, so that is what most of these test.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "cal_types.h"
#include "rc/cal.h"
#include "rc/performance.h"
#include "rc/torque.h"

static rc_torque_manager_t tm;

static rc_perf_input_t base_input(void)
{
    rc_perf_input_t in;
    memset(&in, 0, sizeof(in));
    in.gear = 1u;
    in.rpm = 3000.0f;
    in.speed_kph = 40.0f;
    in.front_speed_kph = 40.0f;
    in.rear_speed_kph = 40.0f;
    in.tps_pct = 10.0f;
    in.dt_s = 0.005f;
    in.mode = 0u;
    return in;
}

static rc_perf_output_t step(const rc_cal_t *cal, rc_perf_state_store_t *store,
                             rc_perf_input_t in, uint32_t now_us)
{
    rc_perf_output_t out;
    rc_torque_init(&tm);
    rc_perf_update(store, cal, &tm, &in, now_us, &out);
    return out;
}

/* Every published scale is an authority figure, and authority may only be reduced.  A
 * feature that can hand back more than it was given can accelerate the engine against a
 * rider's input. */
static void no_performance_feature_can_increase_authority(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_launch_launch_enabled(&cal, true));
    RC_CHECK(rc_cal_set_shift_shift_blip_enabled(&cal, true));
    RC_CHECK(rc_cal_set_traction_traction_enabled(&cal, true));
    RC_CHECK(rc_cal_set_pit_pit_limiter_enabled(&cal, true));

    rc_perf_state_store_t store;
    rc_perf_init(&store);

    rc_perf_input_t in = base_input();
    in.clutch_pressed = true;
    in.launch_requested = true;
    in.rpm = 2000.0f;
    rc_perf_output_t out = step(&cal, &store, in, 1000u);
    RC_CHECK(out.launch_torque_scale <= 1.0f);
    RC_CHECK(out.shift_torque_scale <= 1.0f);
    RC_CHECK(out.traction_torque_scale <= 1.0f);
    RC_CHECK_MSG(out.launch_torque_scale >= 0.0f, "a scale below zero is not an authority");

    /* Pit limiter: the most restrictive case, and it must still be a reduction. */
    in.launch_requested = false;
    in.shift_requested = false;
    in.pit_requested = true;
    in.speed_kph = 200.0f;
    rc_perf_init(&store);
    out = step(&cal, &store, in, 2000u);
    RC_CHECK_EQ_U(out.state, RC_PERF_PIT_LIMIT);
    RC_CHECK_MSG(out.launch_torque_scale <= 1.0f && out.shift_torque_scale <= 1.0f
                     && out.traction_torque_scale <= 1.0f,
                 "a pit limiter must not raise any authority");
}

/* The torque manager must actually see the request.  A performance feature whose whole
 * effect lives in a report field, and never reaches the arbiter, is inert. */
static void requests_reach_the_torque_manager(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_launch_launch_enabled(&cal, true));
    RC_CHECK(rc_cal_set_strategy_shift_request_source(&cal, 1u)); /* a paddle, not a clutch */

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.launch_requested = true;
    in.rpm = 2000.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);

    RC_CHECK_EQ_U(out.state, RC_PERF_LAUNCH_ACTIVE);
    RC_CHECK_MSG(tm.request[RC_REQ_LAUNCH].engaged != 0u,
                 "launch did not register with the torque manager");
    RC_CHECK_MSG(tm.request[RC_REQ_LAUNCH].scale < 1.0f,
                 "launch must request reduced authority, not full");
    /* And the arbiter's answer must reflect it. */
    rc_torque_arbitrate(&tm, 0.0f);
    RC_CHECK_MSG(tm.allowed_scale < 1.0f, "the arbiter ignored the launch request");
}

/* Launch needs a way to be told the clutch is out or a paddle was pressed.  Without one it
 * would fire the moment the button is touched, with the clutch still engaged. */
static void launch_requires_a_shift_source(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_launch_launch_enabled(&cal, true));
    RC_CHECK(rc_cal_set_strategy_shift_request_source(&cal, 0u)); /* no paddle fitted */

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.launch_requested = true;
    in.clutch_pressed = false; /* clutch in, no paddle: there is no safe way to launch */
    in.rpm = 2000.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK_MSG(store.launch_latched == false,
                 "launch latched with no paddle and no clutch signal");
    RC_CHECK_EQ_U(tm.request[RC_REQ_LAUNCH].engaged, 0u);

    /* Clutch pressed is an acceptable source. */
    rc_perf_init(&store);
    rc_torque_init(&tm);
    in.clutch_pressed = true;
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK(store.launch_latched);
}

/* A launch that latches and never releases is a launch that fires on every start.  It must
 * end on timeout and on release, independently. */
static void launch_always_terminates(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_launch_launch_enabled(&cal, true));
    RC_CHECK(rc_cal_set_strategy_shift_request_source(&cal, 1u));
    RC_CHECK(rc_cal_set_launch_launch_timeout_ms(&cal, 2000u));

    /* Released by the rider. */
    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.launch_requested = true;
    in.rpm = 2000.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK(store.launch_latched);
    in.launch_requested = false;
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK_MSG(store.launch_latched == false, "releasing the button must end the launch");

    /* Timed out, with the button still held.  A stuck button is exactly the case a timeout
     * exists for. */
    rc_perf_init(&store);
    rc_torque_init(&tm);
    in.launch_requested = true;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK(store.launch_latched);
    rc_perf_update(&store, &cal, &tm, &in, 1000u + 3000000u, &out);
    RC_CHECK_MSG(store.launch_latched == false, "a held launch must still time out");
}

/* Below the target the launch holds; at or above it, it is armed and no longer requesting
 * the reduction, because the limiter is now doing the holding. */
static void launch_releases_its_request_once_the_target_is_reached(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_launch_launch_enabled(&cal, true));
    RC_CHECK(rc_cal_set_strategy_shift_request_source(&cal, 1u));
    RC_CHECK(rc_cal_set_launch_launch_target_rpm(&cal, 4000.0f));

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.launch_requested = true;
    in.rpm = 2000.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK_EQ_U(out.state, RC_PERF_LAUNCH_ACTIVE);
    RC_CHECK(tm.request[RC_REQ_LAUNCH].engaged != 0u);

    rc_torque_init(&tm);
    in.rpm = 4500.0f;
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK_EQ_U(out.state, RC_PERF_LAUNCH_ARMED);
    RC_CHECK_MSG(tm.request[RC_REQ_LAUNCH].engaged == 0u,
                 "at the launch target the limiter holds, so launch must stop requesting");
}

/* The pit limiter latches and stays latched until the speed is genuinely below the release
 * threshold, so releasing the switch at the limit speed does not immediately restore full
 * power in the rider's hands. */
static void pit_limiter_has_release_hysteresis(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_pit_pit_limiter_enabled(&cal, true));
    RC_CHECK(rc_cal_set_pit_pit_target_speed_kph(&cal, 60.0f));
    RC_CHECK(rc_cal_set_pit_pit_speed_hysteresis_kph(&cal, 5.0f));
    RC_CHECK(rc_cal_set_pit_pit_target_rpm(&cal, 20000u)); /* speed is the limit here */

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.pit_requested = true;
    in.speed_kph = 70.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK(store.pit_latched);

    /* Switch released but still at the limit speed: the limiter holds. */
    rc_torque_init(&tm);
    in.pit_requested = false;
    in.speed_kph = 62.0f;
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK_MSG(store.pit_latched, "releasing the switch at the limit must not resume power");
    RC_CHECK_EQ_U(out.state, RC_PERF_PIT_LIMIT);

    /* Genuinely below the release threshold: it lets go. */
    rc_torque_init(&tm);
    in.speed_kph = 50.0f;
    rc_perf_update(&store, &cal, &tm, &in, 3000u, &out);
    RC_CHECK_MSG(store.pit_latched == false, "below the release threshold it must unlatch");
}

/* Slip is normalised by the driven wheel so it is defined from a standstill, and a stopped
 * driven wheel is "no information", not a division by zero. */
static void slip_is_measured_from_the_driven_wheel(void)
{
    RC_CHECK_NEAR(rc_perf_slip_percent(40.0f, 40.0f), 0.0f, 1e-3);
    /* The front rolling faster than the rear is positive slip. */
    RC_CHECK(rc_perf_slip_percent(50.0f, 40.0f) > 0.0f);
    RC_CHECK_NEAR(rc_perf_slip_percent(50.0f, 40.0f), 25.0f, 1e-3);
    /* Locked rear wheel: the honest answer is "no information", not infinity. */
    RC_CHECK_NEAR(rc_perf_slip_percent(30.0f, 0.0f), 0.0f, 1e-3);
}

/* Traction control needs two wheels to compare.  With one wheel fitted, "slip" is a
 * comparison against a number that does not exist, and acting on it would be noise. */
static void traction_needs_two_wheels_and_real_slip(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_traction_traction_enabled(&cal, true));
    RC_CHECK(rc_cal_set_strategy_wheel_speed_channel_count(&cal, 1u));
    RC_CHECK(rc_cal_set_traction_traction_slip_target_pct(&cal, 8.0f));
    RC_CHECK(rc_cal_set_traction_traction_min_slip_to_act(&cal, 3.0f));

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.front_speed_kph = 60.0f;
    in.rear_speed_kph = 40.0f; /* 50 % slip, if it were measurable at all */
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK_MSG(tm.request[RC_REQ_TRACTION].engaged == 0u,
                 "traction acted with only one wheel fitted");

    /* Two wheels, and the slip is below the minimum: it must stay out of the way. */
    RC_CHECK(rc_cal_set_strategy_wheel_speed_channel_count(&cal, 2u));
    rc_perf_init(&store);
    rc_torque_init(&tm);
    in.front_speed_kph = 41.0f;
    in.rear_speed_kph = 40.0f; /* 2.5 %, below the 3 % minimum */
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK(tm.request[RC_REQ_TRACTION].engaged == 0u);

    /* Two wheels and real slip: it engages, and only reduces. */
    rc_perf_init(&store);
    rc_torque_init(&tm);
    in.front_speed_kph = 60.0f;
    in.rear_speed_kph = 40.0f;
    rc_perf_update(&store, &cal, &tm, &in, 3000u, &out);
    RC_CHECK_EQ_U(out.state, RC_PERF_TRACTION);
    RC_CHECK(tm.request[RC_REQ_TRACTION].engaged != 0u);
    RC_CHECK(out.traction_torque_scale < 1.0f);
    RC_CHECK(out.traction_torque_scale >= 0.0f);
}

/* Engine braking is a retard, and a retard is a small negative number.  A positive value
 * here would be the module adding timing, which is the opposite of engine braking. */
static void engine_braking_only_ever_retards(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_ebt_engine_braking_enabled(&cal, true));
    RC_CHECK(rc_cal_set_ebt_ebt_min_rpm(&cal, 2500.0f));

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.throttle_closed = true;
    in.rpm = 6000.0f;
    rc_perf_output_t out;
    rc_perf_update(&store, &cal, &tm, &in, 1000u, &out);
    RC_CHECK_MSG(out.engine_braking_retard_deg <= 0.0f,
                 "engine braking advanced the spark by %.2f degrees",
                 (double)out.engine_braking_retard_deg);

    /* With the throttle open it is not engine braking at all. */
    rc_perf_init(&store);
    rc_torque_init(&tm);
    in.throttle_closed = false;
    rc_perf_update(&store, &cal, &tm, &in, 2000u, &out);
    RC_CHECK_NEAR(out.engine_braking_retard_deg, 0.0f, 1e-6);
}

/* With everything switched off, a performance module must be completely inert.  A feature
 * flag that does not actually disable the feature is worse than having the feature. */
static void everything_disabled_is_completely_inert(void)
{
    const rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK_EQ_U(cal.launch_launch_enabled, 0u);
    RC_CHECK_EQ_U(cal.traction_traction_enabled, 0u);
    RC_CHECK_EQ_U(cal.pit_pit_limiter_enabled, 0u);
    RC_CHECK_EQ_U(cal.shift_shift_blip_enabled, 0u);
    RC_CHECK_EQ_U(cal.ebt_engine_braking_enabled, 0u);

    rc_perf_state_store_t store;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_input_t in = base_input();
    in.launch_requested = true;
    in.pit_requested = true;
    in.shift_requested = true;
    in.clutch_pressed = true;
    in.throttle_closed = true;
    in.rpm = 9000.0f;
    in.speed_kph = 250.0f;
    rc_perf_output_t out;
    for (uint32_t t = 1000u; t < 20000u; t += 1000u) {
        rc_torque_init(&tm);
        rc_perf_update(&store, &cal, &tm, &in, t, &out);
        RC_CHECK_MSG(tm.request[RC_REQ_LAUNCH].engaged == 0u, "launch acted while disabled");
        RC_CHECK_MSG(tm.request[RC_REQ_TRACTION].engaged == 0u, "TC acted while disabled");
        RC_CHECK_MSG(tm.request[RC_REQ_SHIFT].engaged == 0u, "shift acted while disabled");
        RC_CHECK_MSG(out.launch_torque_scale == 1.0f, "a disabled feature reported a scale");
        RC_CHECK_MSG(out.shift_torque_scale == 1.0f, "a disabled feature reported a scale");
        RC_CHECK_MSG(out.traction_torque_scale == 1.0f, "a disabled feature reported a scale");
        RC_CHECK_MSG(store.launch_latched == false, "a disabled feature latched");
        RC_CHECK_MSG(store.pit_latched == false, "a disabled feature latched");
        RC_CHECK_NEAR(out.engine_braking_retard_deg, 0.0f, 1e-6);
    }
}

/* Null arguments must be survivable: this is called from the main loop, and a crash in the
 * main loop is a crash with the engine running. */
static void null_arguments_are_survivable(void)
{
    rc_perf_state_store_t store;
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_perf_input_t in = base_input();
    rc_perf_output_t out;
    rc_perf_init(&store);
    rc_torque_init(&tm);
    rc_perf_update(NULL, &cal, &tm, &in, 1000u, &out);
    rc_perf_update(&store, NULL, &tm, &in, 1000u, &out);
    rc_perf_update(&store, &cal, NULL, &in, 1000u, &out);
    rc_perf_update(&store, &cal, &tm, NULL, 1000u, &out);
    rc_perf_update(&store, &cal, &tm, &in, 1000u, NULL);
    RC_CHECK(1); /* reaching here without a crash is the assertion */
}

void test_performance(void)
{
    RC_TEST_CASE("no/performance/feature/can/increase/authority",
                 no_performance_feature_can_increase_authority);
    RC_TEST_CASE("requests/reach/the/torque/manager", requests_reach_the_torque_manager);
    RC_TEST_CASE("launch/requires/a/shift/source", launch_requires_a_shift_source);
    RC_TEST_CASE("launch/always/terminates", launch_always_terminates);
    RC_TEST_CASE("launch/releases/its/request/once/the/target/is/reached",
                 launch_releases_its_request_once_the_target_is_reached);
    RC_TEST_CASE("pit/limiter/has/release/hysteresis", pit_limiter_has_release_hysteresis);
    RC_TEST_CASE("slip/is/measured/from/the/driven/wheel", slip_is_measured_from_the_driven_wheel);
    RC_TEST_CASE("traction/needs/two/wheels/and/real/slip",
                 traction_needs_two_wheels_and_real_slip);
    RC_TEST_CASE("engine/braking/only/ever/retards", engine_braking_only_ever_retards);
    RC_TEST_CASE("everything/disabled/is/completely/inert", everything_disabled_is_completely_inert);
    RC_TEST_CASE("null/arguments/are/survivable", null_arguments_are_survivable);
}
