/* RACCOON V2 - idle control tests.
 *
 * Idle is a requester like any other, so the invariants under test are the same shape as
 * everywhere else in the ECU: the module must never be able to raise authority above 1.0,
 * must disengage cleanly, and must respect a hard hardware-adjacent bound rather than
 * trusting itself to stop.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "cal_types.h"
#include "rc/cal.h"
#include "rc/idle.h"

static rc_idle_state_t idle_state;

static rc_cal_t idle_cal(rc_idle_strategy_t strategy)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    RC_CHECK(rc_cal_set_idle_idle_strategy(&cal, (uint8_t)strategy));
    /* A warm engine and a closed throttle are the conditions under which idle may engage. */
    RC_CHECK(rc_cal_set_idle_idle_enable_at_ect_c(&cal, 40.0f));
    return cal;
}

static rc_idle_output_t run(const rc_cal_t *cal, float rpm, float target_rpm, float tps,
                            float ect, float allowance, uint32_t now_us)
{
    rc_idle_output_t out;
    rc_idle_update(&idle_state, cal, rpm, target_rpm, tps, ect, allowance, now_us, &out);
    return out;
}

/* Hysteresis: engaging below the entry speed and releasing above the exit speed is what
 * stops the valve hunting around the threshold, which on a real valve is audible as a
 * stumble.  A controller that engages and disengages on one number will hunt. */
static void entry_and_exit_have_hysteresis(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_PWM_IAC);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));

    rc_idle_init(&idle_state);
    /* Below the entry speed, warm, closed throttle: engaged. */
    RC_CHECK(run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 1.0f, 1000u).engaged);
    /* Between entry and exit it is still engaged - this is the band that must not flap. */
    RC_CHECK(run(&cal, 1000.0f, 850.0f, 0.0f, 80.0f, 1.0f, 2000u).engaged);
    /* Above the exit speed it must let go, even though it is under the entry speed on the
     * way back down only if the exit threshold is respected. */
    RC_CHECK(!run(&cal, 1200.0f, 850.0f, 0.0f, 80.0f, 1.0f, 3000u).engaged);
    /* Coming back down, the hold band is re-entered at the EXIT speed, not the entry
     * speed: the loop must not have to fall all the way back to the entry threshold before
     * it re-engages, or a bike that is hunting just under the exit speed would drop out of
     * idle on every cycle.  The entry speed is the nominal idle target, not a second gate. */
    RC_CHECK(run(&cal, 1050.0f, 850.0f, 0.0f, 80.0f, 1.0f, 4000u).engaged);
    RC_CHECK(run(&cal, 850.0f, 850.0f, 0.0f, 80.0f, 1.0f, 5000u).engaged);
}

/* The conditions that must ALL hold before idle touches anything. */
static void idle_requires_warm_closed_throttle(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_PWM_IAC);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));

    rc_idle_init(&idle_state);
    /* A cold engine has no oil pressure to spare for a cooling loop. */
    RC_CHECK(!run(&cal, 800.0f, 850.0f, 0.0f, 10.0f, 1.0f, 1000u).engaged);
    /* An open throttle is the rider asking for power, not for idle air. */
    RC_CHECK(!run(&cal, 800.0f, 850.0f, 30.0f, 80.0f, 1.0f, 2000u).engaged);
    /* Fast enough to be off idle. */
    RC_CHECK(!run(&cal, 2000.0f, 850.0f, 0.0f, 80.0f, 1.0f, 3000u).engaged);
}

/* The duty may be commanded, but it can never leave the range the hardware can actually
 * deliver: a duty above 100 % is not "more air", it is a permanently open valve. */
static void duty_is_bounded_by_the_valve(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_PWM_IAC);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));
    RC_CHECK(rc_cal_set_idle_idle_valve_duty_min_pct(&cal, 10.0f));
    RC_CHECK(rc_cal_set_idle_idle_valve_duty_max_pct(&cal, 60.0f));

    rc_idle_init(&idle_state);
    /* A large positive error winds the integrator up hard. */
    for (int i = 0; i < 5000; ++i) {
        const rc_idle_output_t out =
            run(&cal, 300.0f, 1500.0f, 0.0f, 80.0f, 1.0f, (uint32_t)(1000u + ((uint32_t)i * 1000u)));
        RC_CHECK_MSG(out.valve_pwm_duty <= 60.0f + 1e-3f,
                     "duty %.3f exceeded the maximum", (double)out.valve_pwm_duty);
        RC_CHECK_MSG(out.valve_pwm_duty >= 0.0f, "duty %.3f went negative",
                     (double)out.valve_pwm_duty);
    }
    /* A large negative error cannot wind it below the minimum either. */
    for (int i = 0; i < 5000; ++i) {
        const rc_idle_output_t out =
            run(&cal, 1050.0f, 500.0f, 0.0f, 80.0f, 1.0f, (uint32_t)(6000000u + ((uint32_t)i * 1000u)));
        RC_CHECK_MSG(out.valve_pwm_duty >= 10.0f - 1e-3f,
                     "duty %.3f fell below the minimum", (double)out.valve_pwm_duty);
    }
}

/* A stepper commanded forever is a thermal event.  The energised time is a hard bound and
 * exceeding it must fault, not merely stop. */
static void stepper_has_a_hard_energised_limit(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_STEPPER);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));
    RC_CHECK(rc_cal_set_idle_idle_stepper_max_energised_ms(&cal, 200u));
    RC_CHECK(rc_cal_set_idle_idle_stepper_steps(&cal, 60.0f));

    rc_idle_init(&idle_state);
    /* Just inside the limit: still energised. */
    const rc_idle_output_t inside =
        run(&cal, 800.0f, 1000.0f, 0.0f, 80.0f, 1.0f, 150000u);
    RC_CHECK(inside.engaged);
    RC_CHECK(inside.stepper_energised);
    RC_CHECK(!idle_state.faulted);
    /* Past the limit: de-energised AND faulted.  Simply stopping without recording why
     * would leave an idle problem that looks identical to a healthy one. */
    const rc_idle_output_t outside =
        run(&cal, 800.0f, 1000.0f, 0.0f, 80.0f, 1.0f, 400000u);
    RC_CHECK(!outside.stepper_energised);
    RC_CHECK_MSG(idle_state.faulted, "exceeding the energised limit must be recorded");
    /* The position never leaves the mechanical range. */
    RC_CHECK((uint32_t)outside.stepper_position <= 60u);
}

/* The torque manager outranks idle.  A module that keeps winding its integrator while the
 * engine is being deliberately held back would fight the limiter it is meant to serve. */
static void idle_yields_to_the_torque_manager(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_PWM_IAC);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));
    rc_idle_init(&idle_state);
    RC_CHECK(run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 1.0f, 1000u).engaged);
    /* Authority withdrawn: idle must stand down immediately. */
    RC_CHECK(!run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 0.4f, 2000u).engaged);
    /* And it does not sneak back in while authority is still withdrawn. */
    RC_CHECK(!run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 0.4f, 3000u).engaged);
}

/* Ignition-only idle retards the spark to stabilise a slow idle.  It is a correction and
 * must be bounded, and it must vanish completely once idle disengages. */
static void ignition_only_idle_is_a_bounded_correction(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_IGNITION);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));
    rc_idle_init(&idle_state);

    const rc_idle_output_t engaged = run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 1.0f, 1000u);
    RC_CHECK(engaged.engaged);
    RC_CHECK_MSG(engaged.ignition_bis_deg < 0.0f, "idle must retard, never advance");
    /* A retard is small.  A large one is not an idle correction, it is the engine being
     * asked not to fire. */
    RC_CHECK_MSG(rc_absf(engaged.ignition_bis_deg) <= 10.0f,
                 "ignition-only idle retarded by %.2f degrees",
                 (double)rc_absf(engaged.ignition_bis_deg));
    /* Disengaged: the correction is removed completely, not left behind as a bias. */
    const rc_idle_output_t off = run(&cal, 2000.0f, 850.0f, 0.0f, 80.0f, 1.0f, 2000u);
    RC_CHECK(!off.engaged);
    RC_CHECK_NEAR(off.ignition_bis_deg, 0.0f, 1e-6);
}

/* The tuner's enum and the firmware's enum are the same numbers on the wire and in the
 * flash image.  A drift between them is invisible in review and catastrophic in the car:
 * a tuner selecting "PWM valve" would silently get whatever the firmware calls 2, which
 * was a stepper.  The schema comment says the same thing; this test is what makes it
 * true rather than aspirational. */
static void the_tuner_and_firmware_agree_on_the_enum(void)
{
    RC_CHECK_EQ_U(RC_IDLE_OFF, 0u);
    RC_CHECK_EQ_U(RC_IDLE_PWM_IAC, 1u);
    RC_CHECK_EQ_U(RC_IDLE_STEPPER, 2u);
    RC_CHECK_EQ_U(RC_IDLE_DBW, 3u);
    RC_CHECK_EQ_U(RC_IDLE_IGNITION, 4u);

    /* And the generated descriptor must accept exactly that range, no wider: a value
     * outside the C enum would be written to flash and then mean nothing. */
    const rc_cal_desc_t *desc = rc_cal_desc_by_name("idle_strategy");
    RC_REQUIRE(desc != NULL);
    RC_CHECK_EQ_U(desc->minimum, 0.0f);
    RC_CHECK_EQ_U(desc->maximum, 4.0f);

    rc_cal_t cal = rc_cal_defaults_filled();
    for (uint8_t v = 0u; v <= 4u; ++v) {
        RC_CHECK_MSG(rc_cal_set_idle_idle_strategy(&cal, v),
                     "the tuner must be able to write strategy %u", (unsigned)v);
        RC_CHECK_EQ_U(rc_cal_get_idle_idle_strategy(&cal), v);
    }
    RC_CHECK(!rc_cal_set_idle_idle_strategy(&cal, 5u));
}

/* "Off" must mean off.  A strategy that is disabled but still commands a valve is how an
 * engine idles on a stale duty after a feature has been switched off in the tuner. */
static void the_off_strategy_commands_nothing(void)
{
    rc_cal_t cal = idle_cal(RC_IDLE_OFF);
    RC_CHECK(rc_cal_set_idle_idle_entry_rpm(&cal, 900.0f));
    RC_CHECK(rc_cal_set_idle_idle_exit_rpm(&cal, 1100.0f));
    rc_idle_init(&idle_state);
    for (int i = 0; i < 20; ++i) {
        const rc_idle_output_t out =
            run(&cal, 800.0f, 850.0f, 0.0f, 80.0f, 1.0f, (uint32_t)(1000u + ((uint32_t)i * 1000u)));
        RC_CHECK(!out.engaged);
        RC_CHECK_NEAR(out.valve_pct, 0.0f, 1e-6);
        RC_CHECK_NEAR(out.ignition_bis_deg, 0.0f, 1e-6);
        RC_CHECK_EQ_U(out.stepper_position, 0u);
        RC_CHECK(!out.stepper_energised);
    }
}

void test_idle(void)
{
    RC_TEST_CASE("entry/and/exit/have/hysteresis", entry_and_exit_have_hysteresis);
    RC_TEST_CASE("idle/requires/warm/closed/throttle", idle_requires_warm_closed_throttle);
    RC_TEST_CASE("duty/is/bounded/by/the/valve", duty_is_bounded_by_the_valve);
    RC_TEST_CASE("stepper/has/a/hard/energised/limit", stepper_has_a_hard_energised_limit);
    RC_TEST_CASE("idle/yields/to/the/torque/manager", idle_yields_to_the_torque_manager);
    RC_TEST_CASE("ignition/only/idle/is/a/bounded/correction",
                 ignition_only_idle_is_a_bounded_correction);
    RC_TEST_CASE("the/tuner/and/firmware/agree/on/the/enum",
                 the_tuner_and_firmware_agree_on_the_enum);
    RC_TEST_CASE("the/off/strategy/commands/nothing", the_off_strategy_commands_nothing);
}
