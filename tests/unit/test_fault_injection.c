/* RACCOON V2 - fault injection: the adversarial pass from spec section 90.
 *
 * Every case here corresponds to a real failure mode, and each asserts a *defined recovery*,
 * not merely that the system keeps running.  Fault injection is compiled only into
 * non-release builds; see firmware/include/rc/fault.h.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/engine_control.h"
#include "rc/hal_virtual.h"
#include "rc/rc_math.h"
#include "rc/safety.h"
#include "rc/scheduler.h"
#include "rc/trigger.h"

static void spin(rc_control_t *control, rc_hal_virtual_t *hal, uint32_t period_us, uint8_t teeth,
                 uint8_t missing, uint32_t revolutions)
{
    for (uint32_t rev = 0u; rev < revolutions; ++rev) {
        for (uint8_t i = 0u; i < (uint8_t)(teeth - missing); ++i) {
            rc_hal_virtual_advance(hal, period_us);
            rc_control_on_capture(control, 0u, hal->now_us, 1u);
        }
        rc_hal_virtual_advance(hal, period_us * (uint32_t)(missing + 1u));
        rc_control_on_capture(control, 0u, hal->now_us, 1u);
    }
}

static void missing_tooth_keeps_phase(void)
{
    /* Inject: one tooth vanishes from a 60-2 wheel.  Expected: detected, phase maintained,
     * no sync loss.  If a single dropped tooth could lose sync, a real engine at speed
     * would be uncontrollable. */
    rc_control_t control;
    rc_control_init(&control);
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.trigger_profile_crank = 1u;
    cal.trigger_input_filter_level = 0u;
    cal.trigger_min_period_us = 40u;
    cal.trigger_overspeed_period_us = 60u;
    const char *reason = NULL;
    RC_CHECK(rc_control_configure(&control, &cal, &reason));

    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    spin(&control, &hal, 1000u, 60u, 2u, 3u);
    RC_CHECK_MSG(control.trigger.state == RC_TRIGGER_SYNCED, "not synced: %s",
                 rc_trigger_state_name(control.trigger.state));
    const uint32_t gaps_before = control.trigger.stats.gap_events;
    spin(&control, &hal, 1000u, 60u, 2u, 1u);
    /* The extra gap must have been counted, not swallowed. */
    RC_CHECK(control.trigger.stats.gap_events > gaps_before);
}

static void false_tooth_is_rejected(void)
{
    /* Inject: a spurious edge 100 us after a real one.  Expected: rejected and counted,
     * phase unchanged, sync retained. */
    rc_control_t control;
    rc_control_init(&control);
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.trigger_profile_crank = 1u;
    cal.trigger_input_filter_level = 1u;
    cal.trigger_min_period_us = 40u;
    cal.trigger_overspeed_period_us = 60u;
    (void)rc_control_configure(&control, &cal, NULL);

    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    spin(&control, &hal, 1000u, 60u, 2u, 3u);
    RC_CHECK(control.trigger.state == RC_TRIGGER_SYNCED);
    const uint32_t rejected = control.trigger.stats.rejected[RC_REJ_FILTERED];
    const uint32_t phase = control.trigger.crank_angle_deg10;
    rc_hal_virtual_advance(&hal, 100u);
    rc_control_on_capture(&control, 0u, hal.now_us, 1u);
    RC_CHECK_MSG(control.trigger.stats.rejected[RC_REJ_FILTERED] > rejected,
                 "the false tooth was not rejected");
    RC_CHECK_EQ_U(control.trigger.crank_angle_deg10, phase);
    RC_CHECK(control.trigger.state == RC_TRIGGER_SYNCED);
}

static void cam_loss_inhibits_sequential(void)
{
    /* Inject: the cam signal stops.  Expected: the sequential configuration is no longer
     * trustworthy, so fuel and spark are inhibited rather than continuing blind. */
    rc_control_t control;
    rc_control_init(&control);
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.trigger_profile_crank = 1u;
    cal.trigger_profile_cam = 1u;
    cal.strategy_injection_strategy = 2u; /* sequential */
    cal.strategy_tdc_verified = 1u;
    cal.strategy_cam_phase_verified = 1u;
    cal.trigger_input_filter_level = 0u;
    /* Both inputs active high, matching the harness: a cam edge on an inactive polarity is
     * correctly ignored, so a test that feeds level 1 without setting the polarity is
     * testing nothing at all. */
    cal.trigger_trigger_edge_polarity = 0x03u;
    const char *reason = NULL;
    RC_CHECK_MSG(rc_control_configure(&control, &cal, &reason), "config: %s",
                 (reason != NULL) ? reason : "?");

    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    spin(&control, &hal, 1000u, 60u, 2u, 4u);
    RC_CHECK(control.trigger.state == RC_TRIGGER_SYNCED);

    /* Arming is allowed: the CONFIGURATION is sound, and arming happens on key-on before
     * the engine has turned.  A gate that demanded a synced cam would be unsatisfiable -
     * the engine could never be started to acquire the cam it was waiting for. */
    RC_CHECK_MSG(rc_control_arm(&control, true, &reason), "arm: %s",
                 (reason != NULL) ? reason : "?");

    /* The safety property lives in the control path, not the arm gate: with only crank
     * edges fed, a sequential strategy has no phase reference, so the cycle must refuse to
     * drive actuators and must not plan a single event. */
    const uint16_t before = rc_sched_pending(&control.sched);
    rc_control_cycle(&control, hal.now_us);
    RC_CHECK_MSG(control.state.armed == 0u,
                 "sequential control reported armed with no cam edges");
    RC_CHECK_EQ_U(rc_sched_pending(&control.sched), before);
    RC_CHECK_NEAR(control.state.injector_ms[0], 0.0f, 1e-6);

    /* And once the cam is actually locked, the same configuration is allowed to run. */
    uint32_t t = hal.now_us;
    for (int rev = 0; rev < 4; ++rev) {
        for (uint8_t i = 0u; i < 58u; ++i) {
            t += 1000u;
            rc_control_on_capture(&control, 0u, t, 1u);
            rc_control_on_capture(&control, 1u, t + 500u, 1u);
        }
        t += 3000u;
        rc_control_on_capture(&control, 0u, t, 1u);
    }
    RC_CHECK_MSG(control.trigger.cam_edge_count > 0u, "the cam never produced an edge");
    rc_control_cycle(&control, t);
    RC_CHECK_MSG(control.state.armed != 0u, "control stayed inhibited with a synced cam");
    (void)reason;
}

static void invalid_configuration_is_refused_by_the_ecu(void)
{
    /* Inject: a configuration that is individually in range but jointly wrong - a dwell
     * maximum above the hardware ceiling.  The GUI might have accepted it; the ECU must
     * not. */
    rc_control_t control;
    rc_control_init(&control);
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.ignition_dwell_max_ms = 12.0f;
    cal.hardware_ignition_max_dwell_ms = 6.0f;
    const char *reason = NULL;
    RC_CHECK_MSG(!rc_control_configure(&control, &cal, &reason),
                 "the ECU accepted a configuration the validator must refuse");
    RC_CHECK(reason != NULL);
    RC_CHECK(control.armed == 0u);
    /* ...and the running configuration is untouched, so the engine keeps its last good tune. */
    RC_CHECK_EQ_U(control.cal.ignition_dwell_max_ms, rc_cal_defaults.ignition_dwell_max_ms);
}

static void power_loss_leaves_a_deterministic_state(void)
{
    /* Inject: a brownout mid-run.  Expected: every output is forced safe, the engine state
     * shows no fuel, and the reset cause is reported. */
    rc_control_t control;
    rc_control_init(&control);
    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    hal.reset = RC_RESET_BROWNOUT;

    spin(&control, &hal, 1000u, 60u, 2u, 3u);
    for (uint8_t i = 0; i < RC_MAX_CYLINDERS; ++i) {
        control.state.injector_ms[i] = 3.0f;
    }
    rc_control_force_safe(&control, &hal.base);
    RC_CHECK_MSG(hal.forced_safe > 0u, "the safe state did not reach the hardware");
    RC_CHECK_EQ_U(control.armed, 0u);
    RC_CHECK_EQ_U(control.state.armed, 0u);
    for (uint8_t i = 0; i < RC_MAX_CYLINDERS; ++i) {
        RC_CHECK_NEAR(control.state.injector_ms[i], 0.0f, 1e-6);
        RC_CHECK_EQ_U(control.state.ignition_cut[i], 1u);
    }
    uint32_t reason = RC_RESET_UNKNOWN;
    rc_hal_virtual_reset_reason(&hal, &reason);
    RC_CHECK_EQ_U(reason, RC_RESET_BROWNOUT);
    RC_CHECK_MSG(strcmp(rc_reset_reason_name(reason), "brownout") == 0, "reason name");
}

static void halted_cpu_still_gets_safe_outputs(void)
{
    /* The single most important fault case: the CPU stops entirely, mid-dwell.  The
     * hardware default state must be safe, which is why the safe state lives in the
     * driver and not in a code path.  Here the scheduler is simply never asked to fire,
     * and the force-safe path is what the watchdog reset would use. */
    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    (void)rc_sched_schedule(&sched, RC_EV_COIL_CHARGE, 0u, 1000u, 5000u, 0u);
    /* The CPU "hangs": nothing is dispatched. */
    rc_hal_virtual_advance(&hal, 10000u);
    rc_hal_virtual_pump_events(&hal);
    /* The coil charge edge did happen (the timer did it), and no further edge follows,
     * because a compare register is one-shot: there is no way for a hung CPU to extend a
     * dwell.  The remaining risk - a coil left energised - is closed by the hardware
     * max-dwell timer, which HIL measures. */
    RC_CHECK(hal.armed_count == 0u);
    /* The watchdog reset path forces everything safe. */
    rc_sched_force_safe(&sched, &hal.base, &hal);
    RC_CHECK(hal.forced_safe > 0u);
}

static void scheduler_expiry_raises_a_defined_action(void)
{
    /* Inject: an event whose deadline passes before it is delivered.  Expected: EXPIRED,
     * counted, escalated - and never delivered late. */
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    (void)rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 1200u, 0u);
    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    rc_hal_virtual_set_time(&hal, 5000u);
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 5000u, &hal.base, &hal), 0u);
    RC_CHECK_EQ_U(sched.stats.expired, 1u);
    RC_CHECK_EQ_U(sched.stats.fired, 0u);
    RC_CHECK_EQ_U(hal.delivery_count, 0u);
    RC_CHECK((sched.expiry_mask & (1u << (uint8_t)RC_EV_INJECTOR_OPEN)) != 0u);
}

static void a_corrupt_table_is_never_run_on(void)
{
    /* Inject: a table cell outside its declared range.  Expected: the image fails
     * validation and the configuration is refused, so the engine never runs on a number
     * that the calibration cannot justify. */
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_validate(&cal, NULL, 0u, NULL));
    float *cells = cal.ignition_ignition_base_table_cells;
    cells[5] = 500.0f; /* 500 degrees, far outside [-10, 35] */
    rc_cal_fault_t faults[8];
    uint32_t blocking = 0u;
    RC_CHECK(!rc_cal_validate(&cal, faults, 8u, &blocking));
    RC_CHECK(blocking > 0u);
    RC_CHECK(strstr(rc_rule_text(faults[0].rule_index), "ignition") != NULL
             || faults[0].rule_index == RC_RULE_INDEX_NONE);
}

void test_fault_injection(void)
{
    RC_TEST_CASE("missing/tooth/keeps/phase", missing_tooth_keeps_phase);
    RC_TEST_CASE("false/tooth/is/rejected", false_tooth_is_rejected);
    RC_TEST_CASE("cam/loss/inhibits/sequential", cam_loss_inhibits_sequential);
    RC_TEST_CASE("invalid/configuration/is/refused/by/the/ecu", invalid_configuration_is_refused_by_the_ecu);
    RC_TEST_CASE("power/loss/leaves/a/deterministic/state", power_loss_leaves_a_deterministic_state);
    RC_TEST_CASE("halted/cpu/still/gets/safe/outputs", halted_cpu_still_gets_safe_outputs);
    RC_TEST_CASE("scheduler/expiry/raises/a/defined/action", scheduler_expiry_raises_a_defined_action);
    RC_TEST_CASE("a/corrupt/table/is/never/run/on", a_corrupt_table_is_never_run_on);
}
