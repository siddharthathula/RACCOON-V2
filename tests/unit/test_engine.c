/* RACCOON V2 - engine model tests.
 *
 * The universal-configuration claim only means something if a 1, a 180 degree twin, a 270
 * degree V-twin, a triple and an inline four all schedule correctly from the same code,
 * and if an invalid configuration is refused rather than clamped.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/engine.h"

static rc_cal_t base_config(void)
{
    rc_cal_t cal = rc_cal_defaults;
    rc_cal_apply_table_fills(&cal);
    rc_cal_apply_axis_defaults(&cal);
    return cal;
}

static void displacement_matches_gt650(void)
{
    /* 78 x 67.8 mm, two cylinders, must come out at the published 647.95 cc.  This is the
     * one geometry in the whole project with a published, primary-source figure, so it is
     * the only place a displacement calculation can be checked against reality. */
    const float cc = rc_engine_displacement_cc(78.0f, 67.8f, 2u);
    RC_CHECK_NEAR(cc, 647.95f, 0.5f);
    /* Swept volume per cylinder, and the four-stroke volumetric flow per revolution. */
    RC_CHECK_NEAR(rc_engine_displacement_cc(78.0f, 67.8f, 2u) * 0.5f, 323.98f, 0.3f);
}

static void clearance_from_compression_ratio(void)
{
    /* V_clearance = V_swept / (CR - 1). */
    const float swept = 323.975f;
    RC_CHECK_NEAR(rc_engine_clearance_cc(swept, 9.5f), swept / 8.5f, 0.01f);
    /* Degenerate inputs return zero rather than a nonsense number. */
    RC_CHECK_NEAR(rc_engine_clearance_cc(0.0f, 9.5f), 0.0f, 1e-6);
    RC_CHECK_NEAR(rc_engine_clearance_cc(swept, 1.0f), 0.0f, 1e-6);
}

static void build_gt650_twin(void)
{
    rc_cal_t cal = base_config();
    rc_engine_t engine;
    const char *reason = NULL;
    RC_CHECK_MSG(rc_engine_build(&cal, &engine, &reason), "build failed: %s",
                 (reason != NULL) ? reason : "?");
    RC_CHECK_EQ_U(engine.cylinder_count, 2u);
    RC_CHECK_NEAR(engine.swept_cc, 323.98f, 0.3f);
    RC_CHECK_NEAR(engine.volumetric_cc_per_rev, 323.98f, 0.3f);
    RC_CHECK_NEAR(engine.clearance_cc, 38.1f, 0.5f);
    /* Firing interval on a 180 degree twin is 360 degrees; both cylinders fire within
     * one 720 degree cycle. */
    RC_CHECK_NEAR(engine.firing_angle_deg[0], 0.0f, 1e-3);
    RC_CHECK_NEAR(engine.firing_angle_deg[1], 360.0f, 1e-3);
    /* Sequential: one injector and one ignition channel per cylinder. */
    cal.strategy_injection_strategy = 2u; /* sequential */
    cal.ignition_ignition_strategy = 0u; /* sequential */
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));
    RC_CHECK_EQ_U(rc_engine_injector_for(&engine, 0u), 0u);
    RC_CHECK_EQ_U(rc_engine_injector_for(&engine, 1u), 1u);
    RC_CHECK_EQ_U(rc_engine_ignition_channel_for(&engine, 1u), 1u);
}

static void universal_firing_intervals(void)
{
    /* The firing interval is always 720/cylinders, whatever the layout: that single fact
     * is why there is no per-engine code. */
    const struct {
        uint8_t cylinders;
        uint32_t layout;
        float interval;
    } cases[] = {
        {1u, 0u, 720.0f},  /* single */
        {2u, 1u, 360.0f},  /* parallel 180 */
        {2u, 2u, 360.0f},  /* V-twin 270: same firing interval, different mechanical phase */
        {3u, 0u, 240.0f},  /* triple */
        {4u, 3u, 180.0f},  /* inline four */
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rc_cal_t cal = base_config();
        cal.engine_cylinder_count = cases[i].cylinders;
        cal.engine_crank_layout = (uint8_t)cases[i].layout;
        RC_CHECK(rc_cal_set_engine_bore_mm(&cal, 70.0f));
        RC_CHECK(rc_cal_set_engine_stroke_mm(&cal, 60.0f));
        RC_CHECK(rc_cal_set_engine_displacement_cc(&cal, 0.0f));
        rc_engine_t engine;
        const char *reason = NULL;
        RC_CHECK_MSG(rc_engine_build(&cal, &engine, &reason),
                     "cylinders=%u layout=%u: %s", cases[i].cylinders, cases[i].layout,
                     (reason != NULL) ? reason : "?");
        /* The invariant is the SPACING between consecutive firing positions, not the
         * absolute position: cylinder 0 always sits at the reference and each subsequent
         * cylinder is one firing interval after it.  Asserting cylinder 1 directly would
         * not even be well defined for the single-cylinder case, which has no cylinder 1
         * - an out-of-range index must read 0, not the last valid value. */
        RC_CHECK_NEAR(rc_engine_firing_angle_deg(&engine, 0u), 0.0f, 1e-3);
        if (cases[i].cylinders > 1u) {
            RC_CHECK_NEAR(rc_engine_firing_angle_deg(&engine, 1u), cases[i].interval, 1e-3);
        }
        for (uint8_t c = 1u; c < cases[i].cylinders; ++c) {
            const float spacing = rc_engine_firing_angle_deg(&engine, c)
                - rc_engine_firing_angle_deg(&engine, (uint8_t)(c - 1u));
            RC_CHECK_MSG(fabs((double)spacing - (double)cases[i].interval) < 1e-3,
                         "cylinders=%u layout=%u: spacing at %u was %.3f, expected %.3f",
                         cases[i].cylinders, cases[i].layout, c, (double)spacing,
                         (double)cases[i].interval);
        }
        /* An index past the last cylinder must be refused, not aliased. */
        RC_CHECK_NEAR(rc_engine_firing_angle_deg(&engine, cases[i].cylinders), 0.0f, 1e-3);
    }
}

static void custom_firing_angles(void)
{
    rc_cal_t cal = base_config();
    cal.engine_cylinder_count = 2u;
    cal.engine_crank_layout = 5u; /* custom */
    /* Written through the setters: the item is stored at 0.01 deg, so assigning an
     * engineering value to the raw field would command 2.75 deg instead of 275. */
    RC_CHECK(rc_cal_set_engine_custom_firing_angle_0(&cal, 0.0f));
    RC_CHECK(rc_cal_set_engine_custom_firing_angle_1(&cal, 275.0f)); /* odd secondary, V-twin */
    rc_engine_t engine;
    const char *reason = NULL;
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));
    RC_CHECK_NEAR(engine.firing_angle_deg[1], 275.0f, 1e-3);
    /* An out-of-cycle angle is refused rather than silently wrapped. */
    /* An out-of-cycle angle is refused by the setter AND by the engine build, so it can
     * never reach the scheduler even if a caller skips the setter. */
    RC_CHECK(!rc_cal_set_engine_custom_firing_angle_1(&cal, 800.0f));
    RC_CHECK_NEAR(rc_cal_get_engine_custom_firing_angle_1(&cal), 275.0f, 1e-3);
    cal.engine_custom_firing_angle_1 = 80000.0f; /* forced past the setter, on purpose */
    RC_CHECK(!rc_engine_build(&cal, &engine, &reason));
    RC_CHECK(reason != NULL);
}

static void rejects_invalid_configurations(void)
{
    rc_cal_t cal = base_config();
    rc_engine_t engine;
    const char *reason = NULL;

    /* Zero cylinders. */
    cal.engine_cylinder_count = 0u;
    RC_CHECK_MSG(!rc_engine_build(&cal, &engine, &reason), "0 cylinders must be refused");
    RC_CHECK(strstr(reason, "1..4") != NULL);

    /* Five cylinders: outside the product range, refused rather than truncated. The
     * restore after this one is what the next case needs; there is deliberately no restore
     * between the two, because nothing reads the count in between. */
    cal.engine_cylinder_count = 5u;
    RC_CHECK(!rc_engine_build(&cal, &engine, &reason));
    cal.engine_cylinder_count = 2u;

    /* A firing order that is not a permutation: this is the "inject into an exhaust
     * stroke" mistake, so it must be impossible to build. */
    cal.engine_firing_order_cyl0 = 1u;
    cal.engine_firing_order_cyl1 = 1u;
    cal.engine_firing_order_cyl2 = 2u;
    cal.engine_firing_order_cyl3 = 3u;
    RC_CHECK_MSG(!rc_engine_build(&cal, &engine, &reason), "duplicate firing order refused");
    RC_CHECK(strstr(reason, "permutation") != NULL);

    /* An index beyond the cylinder count. */
    cal.engine_firing_order_cyl0 = 0u;
    cal.engine_firing_order_cyl1 = 3u; /* cylinder 3 does not exist on a twin */
    RC_CHECK(!rc_engine_build(&cal, &engine, &reason));
    cal.engine_firing_order_cyl1 = 1u;

    /* Non-finite geometry. */
    cal.engine_bore_mm = NAN;
    RC_CHECK(!rc_engine_build(&cal, &engine, &reason));
    RC_CHECK(rc_cal_set_engine_bore_mm(&cal, 78.0f));

    /* A compression ratio below 1 is physically impossible.  It is refused twice over:
     * the item's own range rejects the write, and - because a caller may reach the raw
     * image without the setter - the engine build rejects the value as well. */
    RC_CHECK(!rc_cal_set_engine_compression_ratio(&cal, 0.5f));
    cal.engine_compression_ratio = 50.0f; /* 0.5:1 forced straight into the raw image */
    RC_CHECK(!rc_engine_build(&cal, &engine, &reason));
    RC_CHECK(rc_cal_set_engine_compression_ratio(&cal, 9.5f));
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));

    /* A stated displacement that disagrees with bore x stroke by more than 2 %. */
    RC_CHECK(rc_cal_set_engine_displacement_cc(&cal, 1000.0f));
    RC_CHECK_MSG(!rc_engine_build(&cal, &engine, &reason), "disagreement must be refused");
    RC_CHECK(strstr(reason, "2 percent") != NULL);
    /* ...and agreeing within tolerance is accepted. */
    RC_CHECK(rc_cal_set_engine_displacement_cc(&cal, 648.0f));
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));
}

static void strategy_assignment(void)
{
    rc_cal_t cal = base_config();
    rc_engine_t engine;
    const char *reason = NULL;

    /* Wasted spark: pairs share a channel. */
    cal.engine_crank_layout = 3u;
    cal.engine_cylinder_count = 4u;
    cal.engine_bore_mm = 80.0f;
    cal.engine_stroke_mm = 70.0f;
    cal.engine_displacement_cc = 0.0f;
    cal.ignition_ignition_strategy = 1u; /* wasted spark */
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));
    RC_CHECK_EQ_U(rc_engine_ignition_channel_for(&engine, 0u), 0u);
    RC_CHECK_EQ_U(rc_engine_ignition_channel_for(&engine, 1u), 1u);
    RC_CHECK_EQ_U(rc_engine_ignition_channel_for(&engine, 2u), 0u);
    RC_CHECK_EQ_U(rc_engine_ignition_channel_for(&engine, 3u), 1u);

    /* Batch: every injector fires together on channel 0. */
    cal.strategy_injection_strategy = 0u;
    RC_CHECK(rc_engine_build(&cal, &engine, &reason));
    RC_CHECK_EQ_U(rc_engine_injector_for(&engine, 0u), 0u);
    RC_CHECK_EQ_U(rc_engine_injector_for(&engine, 3u), 0u);

    /* A cylinder index that does not exist returns 0xFF rather than channel 0, so a
     * caller cannot accidentally address another cylinder's injector. */
    RC_CHECK_EQ_U(rc_engine_injector_for(&engine, 4u), 0xFFu);
}

static void phase_source_is_not_fabricated(void)
{
    rc_cal_t cal = base_config();
    cal.strategy_injection_strategy = 2u; /* sequential */
    const char *reason = NULL;

    /* No cam: sequential is physically invalid and must be refused.  The firmware is not
     * allowed to invent a phase source (spec section 12). */
    cal.trigger_profile_cam = 0u;
    cal.strategy_cam_phase_verified = 1u;
    cal.strategy_tdc_verified = 1u;
    RC_CHECK_MSG(!rc_engine_strategy_valid(&cal, &reason), "sequential without a cam refused");
    RC_CHECK(strstr(reason, "phase") != NULL);
    RC_CHECK(!rc_engine_phase_source_valid(&cal));

    /* A cam fitted but the phase not verified on the bench: still refused, because an
     * unverified phase is an assumption. */
    cal.trigger_profile_cam = 1u;
    cal.strategy_cam_phase_verified = 0u;
    RC_CHECK(!rc_engine_phase_source_valid(&cal));
    RC_CHECK(!rc_engine_strategy_valid(&cal, &reason));

    /* Verified: valid. */
    cal.strategy_cam_phase_verified = 1u;
    RC_CHECK(rc_engine_phase_source_valid(&cal));
    RC_CHECK(rc_engine_strategy_valid(&cal, &reason));

    /* A two-teeth-per-revolution cam is a distributor, not a phase reference. */
    cal.trigger_teeth_per_rev = 2u;
    RC_CHECK(!rc_engine_phase_source_valid(&cal));
    cal.trigger_teeth_per_rev = 1u;

    /* Batch needs no phase source at all. */
    cal.strategy_injection_strategy = 0u;
    cal.trigger_profile_cam = 0u;
    RC_CHECK(rc_engine_strategy_valid(&cal, &reason));
}

void test_engine(void)
{
    RC_TEST_CASE("displacement/matches/gt650", displacement_matches_gt650);
    RC_TEST_CASE("clearance/from/compression/ratio", clearance_from_compression_ratio);
    RC_TEST_CASE("build/gt650/twin", build_gt650_twin);
    RC_TEST_CASE("universal/firing/intervals", universal_firing_intervals);
    RC_TEST_CASE("custom/firing/angles", custom_firing_angles);
    RC_TEST_CASE("rejects/invalid/configurations", rejects_invalid_configurations);
    RC_TEST_CASE("strategy/assignment", strategy_assignment);
    RC_TEST_CASE("phase/source/is/not/fabricated", phase_source_is_not_fabricated);
}
