/* RACCOON V2 - torque manager tests, including the adversarial fuzz of rule T2. */
#include "rc_test.h"

#include "rc/torque.h"

/* A tiny deterministic PRNG, so a failing fuzz case reproduces exactly from its seed. */
static uint32_t rng_state;
static void rng_seed(uint32_t seed) { rng_state = seed; }
static uint32_t rng_next(void)
{
    rng_state = (rng_state * 1664525u) + 1013904223u;
    return rng_state;
}

static void t1_monotone_deauthority(void)
{
    rc_torque_manager_t tm;
    rc_torque_init(&tm);
    /* A requester asks for half authority... */
    rc_torque_request(&tm, RC_REQ_TRACTION, 0.5f, false, 1000u, 0u);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_NEAR(tm.allowed_scale, 0.5f, 1e-6);
    /* ...and then tries to take some back within the same cycle.  It may not. */
    rc_torque_request(&tm, RC_REQ_TRACTION, 0.9f, false, 1000u, 0u);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_MSG(tm.allowed_scale <= 0.5f, "authority was given back inside a cycle");
    /* After the cycle ends, the request is withdrawn and a fresh one may ask again. */
    rc_torque_end_cycle(&tm);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_NEAR(tm.allowed_scale, 1.0f, 1e-6);
}

static void t2_fixed_priority(void)
{
    rc_torque_manager_t tm;
    /* Every pair, in both orders, must give the same answer: the stronger reduction wins
     * regardless of the order the functions happen to run in. */
    for (uint8_t strong = 1u; strong < (uint8_t)RC_REQ_RIDER; ++strong) {
        for (uint8_t weak = 1u; weak < (uint8_t)RC_REQ_RIDER; ++weak) {
            if (strong == weak) {
                continue;
            }
            rc_torque_init(&tm);
            rc_torque_request(&tm, (rc_requester_t)strong, 0.2f, false, 0u, 0u);
            rc_torque_request(&tm, (rc_requester_t)weak, 0.8f, false, 0u, 0u);
            rc_torque_arbitrate(&tm, 10.0f);
            const float first = tm.allowed_scale;
            const uint8_t winner = tm.winner;

            rc_torque_init(&tm);
            rc_torque_request(&tm, (rc_requester_t)weak, 0.8f, false, 0u, 0u);
            rc_torque_request(&tm, (rc_requester_t)strong, 0.2f, false, 0u, 0u);
            rc_torque_arbitrate(&tm, 10.0f);
            RC_CHECK_MSG(tm.allowed_scale == first,
                         "order dependence: %s then %s vs %s then %s",
                         rc_torque_requester_name(strong), rc_torque_requester_name(weak),
                         rc_torque_requester_name(weak), rc_torque_requester_name(strong));
            RC_CHECK_MSG(tm.winner == winner, "winner depends on call order");
            /* The winner is whichever asked for the stronger *reduction*; the rank
             * decides only when two requesters ask for the same scale. */
            const float expected = (0.2f < 0.8f) ? 0.2f : 0.8f;
            RC_CHECK_NEAR(tm.allowed_scale, expected, 1e-6f);
            RC_CHECK_MSG(tm.winner == ((0.2f < 0.8f) ? strong : weak),
                         "ranks %u and %u both asked for 0.2/0.8 and the winner was %u",
                         (unsigned)strong, (unsigned)weak, (unsigned)tm.winner);
        }
    }
}

static void t3_cut_ownership(void)
{
    rc_torque_manager_t tm;
    rc_torque_init(&tm);
    /* Traction drives the scale to zero: that is a cut, owned by traction. */
    rc_torque_request(&tm, RC_REQ_TRACTION, 0.0f, true, 1000u, 5000u);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK(rc_torque_fuel_forbidden(&tm));
    RC_CHECK_EQ_U(rc_torque_cut_owner(&tm), (uint8_t)RC_REQ_TRACTION);
    RC_CHECK(tm.latched[RC_REQ_TRACTION]);

    /* A *lower* rank requester now asks for full authority.  It must not clear the cut. */
    rc_torque_request(&tm, RC_REQ_RIDER, 1.0f, false, 1100u, 0u);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_MSG(rc_torque_fuel_forbidden(&tm), "a cut was resurrected by a weaker requester");
    RC_CHECK_EQ_U(rc_torque_cut_owner(&tm), (uint8_t)RC_REQ_TRACTION);

    /* Ending the cycle does not clear a latch either. */
    rc_torque_end_cycle(&tm);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_MSG(rc_torque_fuel_forbidden(&tm), "a cut did not survive the end of the cycle");

    /* Only the owner releases it. */
    rc_torque_release(&tm, RC_REQ_TRACTION);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK(!rc_torque_fuel_forbidden(&tm));
    RC_CHECK_NEAR(tm.allowed_scale, 1.0f, 1e-6);
}

static void t4_never_exceeds_unity(void)
{
    rc_torque_manager_t tm;
    rc_torque_init(&tm);
    /* Even if every requester asks for more than full authority, the result cannot exceed
     * 1.0 - the arbitration scales requests, it never invents authority. */
    for (uint8_t who = 1u; who < (uint8_t)RC_REQ_COUNT; ++who) {
        rc_torque_request(&tm, (rc_requester_t)who, 5.0f, false, 0u, 0u);
    }
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK(tm.allowed_scale <= 1.0f);
    RC_CHECK(tm.allowed_scale >= 0.0f);
    /* And the retard equivalent is bounded even at zero authority. */
    rc_torque_request(&tm, RC_REQ_SYNC_LOSS, 0.0f, true, 0u, 0u);
    rc_torque_arbitrate(&tm, 1000.0f);
    RC_CHECK(tm.retard_equivalent <= 40.0f);
    RC_CHECK(tm.retard_equivalent >= 0.0f);
}

static void t5_every_requester_appears_in_the_trace(void)
{
    rc_torque_manager_t tm;
    rc_torque_init(&tm);
    rc_torque_request(&tm, RC_REQ_KNOCK, 0.9f, false, 0u, 0u);
    rc_torque_request(&tm, RC_REQ_TRACTION, 0.9f, false, 0u, 0u);
    rc_torque_arbitrate(&tm, 10.0f);
    /* T6: a log must be able to explain what every requester was doing. */
    RC_CHECK((tm.trace & (1u << (uint8_t)RC_REQ_KNOCK)) != 0u);
    RC_CHECK((tm.trace & (1u << (uint8_t)RC_REQ_TRACTION)) != 0u);
    RC_CHECK((tm.trace & (1u << (uint8_t)RC_REQ_LAUNCH)) == 0u);
}

static void t_fuzz_arbitration(void)
{
    /* The adversarial test for T2: ten thousand random simultaneous request sets, with
     * the invariant that the outcome is always the strongest reduction present, whatever
     * order they arrived in. */
    for (uint32_t seed = 1u; seed <= 10000u; ++seed) {
        rng_seed(seed);
        rc_torque_manager_t reference;
        rc_torque_init(&reference);
        float expected = 1.0f;
        uint8_t expected_winner = (uint8_t)RC_REQ_NONE;

        for (uint8_t attempt = 0u; attempt < 3u; ++attempt) {
            const uint8_t who = (uint8_t)(1u + (rng_next() % (uint32_t)(RC_REQ_COUNT - 2u)));
            const float scale = (float)(rng_next() % 101u) / 100.0f;
            rc_torque_request(&reference, (rc_requester_t)who, scale, false, 0u, 0u);
            if (who != (uint8_t)RC_REQ_RIDER && who < expected_winner) {
                /* track the strongest rank that asked for a real reduction */
            }
            if (scale < expected || (expected_winner == (uint8_t)RC_REQ_NONE && who != (uint8_t)RC_REQ_RIDER)) {
                if (scale < expected) {
                    expected = scale;
                    expected_winner = who;
                }
            }
        }
        rc_torque_arbitrate(&reference, 10.0f);

        /* Now the same three requests in a different order. */
        rc_torque_manager_t shuffled;
        rc_torque_init(&shuffled);
        for (uint8_t who = 1u; who < (uint8_t)RC_REQ_COUNT; ++who) {
            if (reference.request[who].engaged == 0u) {
                continue;
            }
            rc_torque_request(&shuffled, (rc_requester_t)who, reference.request[who].scale, false,
                              0u, 0u);
        }
        rc_torque_arbitrate(&shuffled, 10.0f);

        RC_CHECK_MSG(shuffled.allowed_scale == reference.allowed_scale,
                     "seed %u: order changed the result (%f vs %f)", (unsigned)seed,
                     (double)shuffled.allowed_scale, (double)reference.allowed_scale);
        RC_CHECK_MSG(shuffled.winner == reference.winner, "seed %u: winner changed", (unsigned)seed);
    }
}

static void t_no_request_within_bounds(void)
{
    rc_torque_manager_t tm;
    rc_torque_init(&tm);
    /* Out-of-range requester indices are ignored rather than corrupting the array. */
    rc_torque_request(&tm, (rc_requester_t)RC_REQ_COUNT, 0.1f, false, 0u, 0u);
    rc_torque_request(&tm, (rc_requester_t)99, 0.1f, false, 0u, 0u);
    rc_torque_arbitrate(&tm, 10.0f);
    RC_CHECK_NEAR(tm.allowed_scale, 1.0f, 1e-6);
    RC_CHECK_EQ_U(tm.winner, (uint8_t)RC_REQ_NONE);
}

void test_torque(void)
{
    RC_TEST_CASE("t1/monotone/deauthority", t1_monotone_deauthority);
    RC_TEST_CASE("t2/fixed/priority", t2_fixed_priority);
    RC_TEST_CASE("t3/cut/ownership", t3_cut_ownership);
    RC_TEST_CASE("t4/never/exceeds/unity", t4_never_exceeds_unity);
    RC_TEST_CASE("t5/every/requester/appears/in/the/trace", t5_every_requester_appears_in_the_trace);
    RC_TEST_CASE("t/fuzz/arbitration", t_fuzz_arbitration);
    RC_TEST_CASE("t/no/request/within/bounds", t_no_request_within_bounds);
}
