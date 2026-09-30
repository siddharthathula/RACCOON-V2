/* RACCOON V2 - scheduler tests: the invariants S1..S6 from scheduler.h. */
#include "rc_test.h"

#include "rc/hal_virtual.h"
#include "rc/scheduler.h"

static void s1_no_resurrection(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 2000u, 0u), RC_HAL_OK);
    /* Fire it, then try to schedule exactly the same event again. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 1000u, NULL, NULL), 1u);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 2000u, 0u),
                  RC_HAL_ERR_REFUSED);
    /* And a new plan generation is the only way to get another event on the same channel. */
    rc_sched_new_plan(&sched);
    RC_CHECK(rc_sched_generation(&sched) != 0u);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 2000u, 3000u, 0u), RC_HAL_OK);
}

static void s2_no_duplicates(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_COIL_CHARGE, 1u, 500u, 1500u, 1u), RC_HAL_OK);
    /* The same (type, cylinder, generation) is refused. */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_COIL_CHARGE, 1u, 500u, 1500u, 1u),
                  RC_HAL_ERR_REFUSED);
    RC_CHECK_EQ_U(sched.stats.refused_duplicate, 1u);
    /* A different cylinder is a different channel and is accepted. */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_COIL_CHARGE, 2u, 500u, 1500u, 1u), RC_HAL_OK);
}

static void s3_stale_rejection(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_CLOSE, 0u, 5000u, 6000u, 0u), RC_HAL_OK);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), 1u);
    /* A new plan invalidates everything pending from the previous one: this is how a mode
     * change cannot be followed by a late event from the old strategy. */
    rc_sched_new_plan(&sched);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), 0u);
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 10000u, NULL, NULL), 0u);
    RC_CHECK_EQ_U(sched.stats.fired, 0u);
}

static void s4_expiry_is_explicit(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    /* A deadline already in the past when the event becomes due: it must expire, be
     * counted, and raise the escalation mask - never execute late. */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 900u, 0u), RC_HAL_OK);
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 1000u, NULL, NULL), 0u);
    RC_CHECK_EQ_U(sched.stats.expired, 1u);
    RC_CHECK_EQ_U(sched.stats.fired, 0u);
    RC_CHECK((sched.expiry_mask & (1u << (uint8_t)RC_EV_INJECTOR_OPEN)) != 0u);
    rc_sched_clear_expiry(&sched);
    RC_CHECK_EQ_U(sched.expiry_mask, 0u);
}

static void s5_wrap_safe_time(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    /* Schedule just past the 32-bit wrap. */
    const uint32_t near_wrap = 0xFFFFF000u;
    const uint32_t target = 0x00001000u; /* 8 KiB later, across the wrap */
    const uint32_t deadline = 0x00002000u;
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_COIL_FIRE, 0u, target, deadline, 0u), RC_HAL_OK);
    /* Before the wrap, the event is in the future: nothing fires. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, near_wrap, NULL, NULL), 0u);
    /* After the wrap, it is due and fires exactly once. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, target, NULL, NULL), 1u);
    RC_CHECK_EQ_U(sched.stats.fired, 1u);
    /* And it does not fire again. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, target + 100u, NULL, NULL), 0u);

    /* The helpers agree with the same wrap arithmetic. */
    RC_CHECK_EQ_U(rc_ticks_forward(target, near_wrap), (uint32_t)(target - near_wrap));
    RC_CHECK(rc_deadline_ok(near_wrap, target));
    RC_CHECK(!rc_deadline_ok(target, near_wrap));
}

static void s6_no_channel_collision(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 1u, 1000u, 2000u, 0u), RC_HAL_OK);
    /* A second event for the same channel in one plan is a configuration error. */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 1u, 1200u, 2200u, 3u),
                  RC_HAL_ERR_REFUSED);
    RC_CHECK_EQ_U(sched.stats.refused_collision, 1u);
    /* A different channel on the same hardware type is fine. */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 2u, 1000u, 2000u, 0u), RC_HAL_OK);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), 2u);
}

static void cancel_is_one_way(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    (void)rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 5000u, 0u);
    (void)rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 1u, 1000u, 5000u, 0u);
    (void)rc_sched_schedule(&sched, RC_EV_INJECTOR_CLOSE, 0u, 1200u, 5000u, 0u);
    /* Cancel one cylinder only. */
    RC_CHECK_EQ_U(rc_sched_cancel(&sched, RC_EV_INJECTOR_OPEN, 0), 1u);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), 2u);
    /* Cancel the whole type. */
    RC_CHECK_EQ_U(rc_sched_cancel(&sched, RC_EV_INJECTOR_CLOSE, -1), 1u);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), 1u);
    /* Nothing cancelled ever fires: a fuel cut that could be undone is not a fuel cut. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 10000u, NULL, NULL), 0u);
    RC_CHECK_EQ_U(sched.stats.fired, 0u);
    RC_CHECK_EQ_U(sched.stats.cancelled, 2u);
}

static void queue_overflow_is_bounded(void)
{
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    uint32_t accepted = 0u;
    /* Fill the queue with distinct channels, then prove the overflow is refused rather
     * than overwriting an existing event. */
    for (uint16_t i = 0u; i < RC_SCHEDULER_QUEUE + 8u; ++i) {
        if (rc_sched_schedule(&sched, (rc_event_type_t)(i % (uint16_t)RC_EV_TYPE_COUNT),
                              (uint8_t)(i / (uint16_t)RC_EV_TYPE_COUNT), 1000u, 9000u, 0u)
            == RC_HAL_OK) {
            accepted++;
        }
    }
    RC_CHECK(accepted <= RC_SCHEDULER_QUEUE);
    RC_CHECK(sched.stats.refused_full > 0u);
    RC_CHECK_EQ_U(rc_sched_pending(&sched), (uint16_t)accepted);
}

static void hardware_edges_are_delivered_once(void)
{
    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    rc_scheduler_t sched;
    rc_sched_init(&sched);

    /* An event in the future is armed and held by the "timer". */
    RC_CHECK_EQ_U(rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 5000u, 9000u, 0u), RC_HAL_OK);
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 1000u, &hal.base, &hal), 0u);
    RC_CHECK_EQ_U(hal.armed_count, 1u);
    /* Time passes and the compare arrives: the edge is delivered exactly once. */
    rc_hal_virtual_set_time(&hal, 5000u);
    rc_hal_virtual_pump_events(&hal);
    RC_CHECK_EQ_U(hal.delivery_count, 1u);
    /* The scheduler still believes it is pending, because a compare register is one-shot
     * and the driver retires it: the scheduler must not re-arm it. */
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 5000u, &hal.base, &hal), 0u);
    rc_hal_virtual_pump_events(&hal);
    RC_CHECK_EQ_U(hal.delivery_count, 1u);
}

static void force_safe_touches_every_channel(void)
{
    rc_hal_virtual_t hal;
    rc_hal_virtual_init(&hal);
    rc_scheduler_t sched;
    rc_sched_init(&sched);
    (void)rc_sched_schedule(&sched, RC_EV_INJECTOR_OPEN, 0u, 1000u, 9000u, 0u);
    rc_sched_force_safe(&sched, &hal.base, &hal);
    /* Injectors, coils and the cut outputs are all forced, for every channel. */
    RC_CHECK(hal.forced_safe >= (RC_MAX_INJECTORS + RC_MAX_IGNITION + 2u));
    /* ...and the queue is irrelevant afterwards: nothing is pending to fire. */
    (void)rc_sched_cancel(&sched, RC_EV_INJECTOR_OPEN, -1);
    RC_CHECK_EQ_U(rc_sched_dispatch(&sched, 100000u, &hal.base, &hal), 0u);
}

void test_scheduler(void)
{
    RC_TEST_CASE("s1/no/resurrection", s1_no_resurrection);
    RC_TEST_CASE("s2/no/duplicates", s2_no_duplicates);
    RC_TEST_CASE("s3/stale/rejection", s3_stale_rejection);
    RC_TEST_CASE("s4/expiry/is/explicit", s4_expiry_is_explicit);
    RC_TEST_CASE("s5/wrap/safe/time", s5_wrap_safe_time);
    RC_TEST_CASE("s6/no/channel/collision", s6_no_channel_collision);
    RC_TEST_CASE("cancel/is/one/way", cancel_is_one_way);
    RC_TEST_CASE("queue/overflow/is/bounded", queue_overflow_is_bounded);
    RC_TEST_CASE("hardware/edges/are/delivered/once", hardware_edges_are_delivered_once);
    RC_TEST_CASE("force/safe/touches/every/channel", force_safe_touches_every_channel);
}
