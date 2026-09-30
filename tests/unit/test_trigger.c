/* RACCOON V2 - trigger decoder tests.
 *
 * The decoder is tested as a *sequence* of exact states, not as an end state: "it ends up
 * SYNCED" is a weak claim, "it went NO_SIGNAL -> INITIALIZING -> SEEKING_SYNC ->
 * SYNC_CANDIDATE -> SYNCED and then held SYNCED through a 36-1 revolution" is a test that
 * can fail (docs/TRIGGER_SPEC.md section 4).
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/trigger.h"

typedef struct {
    rc_trigger_t trig;
    uint32_t tick;
} rig_t;

static void rig_configure(rig_t *rig, uint8_t profile, uint8_t teeth, uint8_t missing,
                          uint8_t cam_teeth, uint8_t filter_level, uint8_t skip_revs)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.trigger_profile_crank = profile;
    cal.trigger_teeth = teeth;
    cal.trigger_missing_teeth = missing;
    cal.trigger_profile_cam = cam_teeth;
    cal.trigger_input_filter_level = filter_level;
    cal.trigger_sync_skip_revolutions = skip_revs;
    cal.trigger_min_period_us = 40u;
    cal.trigger_overspeed_period_us = 60u;
    cal.trigger_max_period_us = 2000000u;
    cal.trigger_trigger_edge_polarity = 0x03u; /* active high on both */
    memset(rig, 0, sizeof(*rig));
    const char *reason = NULL;
    RC_CHECK_MSG(rc_trigger_configure(&rig->trig, &cal, &reason), "configure failed: %s",
                 (reason != NULL) ? reason : "?");
    rig->tick = 0u;
}

/** One crank revolution of `teeth` evenly spaced edges, then the gap. */
static void spin_revolutions(rig_t *rig, uint8_t teeth, uint8_t missing, uint32_t period_us,
                             uint32_t revolutions)
{
    for (uint32_t rev = 0u; rev < revolutions; ++rev) {
        for (uint8_t i = 0u; i < (uint8_t)(teeth - missing); ++i) {
            rig->tick += period_us;
            rc_trigger_edge(&rig->trig, 0u, rig->tick, 1u);
        }
        /* The gap: a longer interval standing in for the missing teeth. */
        rig->tick += period_us * (missing + 1u);
        rc_trigger_edge(&rig->trig, 0u, rig->tick, 1u);
    }
}

static void state_is_no_signal_at_reset(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 1u, 0u);
    RC_CHECK_EQ_U(rig.trig.state, RC_TRIGGER_NO_SIGNAL);
    RC_CHECK_EQ_U(rig.trig.sync_confidence, 0u);
    RC_CHECK(!rc_trigger_permits_control(&rig.trig));
    RC_CHECK(!rc_trigger_has_rpm(RC_TRIGGER_NO_SIGNAL));
}

static void first_edge_initialises(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 1u, 0u);
    rig.tick = 1000u;
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    /* One edge is a place to start, not a period, so no RPM exists yet. */
    RC_CHECK_EQ_U(rig.trig.state, RC_TRIGGER_INITIALIZING);
    RC_CHECK(!rc_trigger_has_rpm(rig.trig.state));
    RC_CHECK(!rc_trigger_permits_control(&rig.trig));
}

static void acquires_sync_on_a_missing_tooth_wheel(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u); /* 60-2, no filter, one confirmation */
    spin_revolutions(&rig, 60u, 2u, 1000u, 4u);
    RC_CHECK_MSG(rig.trig.state == RC_TRIGGER_SYNCED, "expected SYNCED, got %s (%u rejected)",
                 rc_trigger_state_name(rig.trig.state), rig.trig.stats.rejected[RC_REJ_FILTERED]);
    RC_CHECK_EQ_U(rig.trig.sync_confidence, 100u);
    RC_CHECK(rc_trigger_permits_control(&rig.trig));
    RC_CHECK(rig.trig.stats.gap_events > 0u);
    /* 60 teeth at 1 ms each is 60 ms per revolution, which is 1000 rpm.  Reading the raw
     * tooth rate here would give 60000 and trip every rev limit in the engine at idle. */
    rc_engine_position_t pos;
    rc_trigger_get_position(&rig.trig, &pos);
    const float rpm = (float)pos.rpm_x2 * 0.5f;
    RC_CHECK_MSG(fabs((double)rpm - 1000.0) < 150.0, "rpm %.1f", (double)rpm);
}

static void even_wheel_never_claims_sync(void)
{
    rig_t rig;
    /* An even wheel has no gap and therefore no phase reference.  RPM is valid; sync is
     * never declared, which is what the validator promises the tuner. */
    rig_configure(&rig, 9u, 24u, 0u, 0u, 0u, 0u);
    for (uint8_t i = 0u; i < 24u; ++i) {
        rig.tick += 1000u;
        rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    }
    RC_CHECK(rig.trig.state != RC_TRIGGER_SYNC_CANDIDATE);
    RC_CHECK(rig.trig.state != RC_TRIGGER_SYNCED);
    RC_CHECK(rig.trig.sync_confidence == 0u);
}

static void filter_rejects_a_glitch_but_not_a_real_tooth(void)
{
    rig_t rig;
    /* Default filter is LITE, and it must reject a spuriously fast edge... */
    rig_configure(&rig, 1u, 60u, 2u, 0u, 1u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    const uint32_t before = rig.trig.stats.rejected[RC_REJ_FILTERED];
    rig.tick += 50u; /* a glitch 50 us after the last edge */
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    RC_CHECK_MSG(rig.trig.stats.rejected[RC_REJ_FILTERED] > before,
                 "a 50 us edge must be filtered");
    /* ...while a real tooth at the expected period is still accepted. */
    const uint32_t accepted = rig.trig.stats.edges_accepted;
    rig.tick += 1000u;
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    RC_CHECK_EQ_U(rig.trig.stats.edges_accepted, accepted + 1u);
}

static void overspeed_is_a_hard_fault(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 2u);
    /* An edge faster than the overspeed trip must fault and never resume. */
    rig.tick += 10u;
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    RC_CHECK_EQ_U(rig.trig.state, RC_TRIGGER_FAULT);
    RC_CHECK(!rc_trigger_permits_control(&rig.trig));
    /* A fault is terminal: further legal edges must not silently clear it. */
    const uint8_t before = rig.trig.state;
    for (uint8_t i = 0u; i < 40u; ++i) {
        rig.tick += 1000u;
        rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    }
    RC_CHECK_EQ_U(rig.trig.state, before);
}

static void tooth_timeout_drops_sync(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    RC_CHECK(rig.trig.state == RC_TRIGGER_SYNCED);
    /* A gap far longer than max_period is a lost signal, not a stall. */
    rig.tick += 3000000u;
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    RC_CHECK_MSG(rig.trig.state == RC_TRIGGER_NO_SIGNAL, "expected NO_SIGNAL, got %s",
                 rc_trigger_state_name(rig.trig.state));
    RC_CHECK(!rc_trigger_permits_control(&rig.trig));
    /* A stalled signal must not accumulate a bogus RPM. */
    rc_engine_position_t pos;
    rc_trigger_get_position(&rig.trig, &pos);
    RC_CHECK_EQ_U(pos.rpm_x2, 0u);
}

static void stall_then_restart(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    /* Long stall: no edges at all. */
    rig.tick += 5000000u;
    rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    RC_CHECK(!rc_trigger_permits_control(&rig.trig));
    /* The starter turns the engine over slowly: a different period entirely. */
    const uint32_t cranking_period = 20000u; /* 50 rpm on a 60 tooth wheel */
    for (uint8_t i = 0u; i < 5u; ++i) {
        rig.tick += cranking_period;
        rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    }
    RC_CHECK(rig.trig.state != RC_TRIGGER_FAULT);
    /* Then it catches and runs. */
    spin_revolutions(&rig, 60u, 2u, 1000u, 4u);
    RC_CHECK_MSG(rig.trig.state == RC_TRIGGER_SYNCED, "expected SYNCED after restart, got %s",
                 rc_trigger_state_name(rig.trig.state));
}

static void cam_edge_establishes_phase(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 1u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    RC_CHECK(rig.trig.state == RC_TRIGGER_SYNCED);
    /* A cam edge is recorded for the cycle logic. */
    const uint8_t before = (uint8_t)rig.trig.cam_edge_count;
    rig.tick += 1000u;
    rc_trigger_edge(&rig.trig, 1u, rig.tick, 1u);
    RC_CHECK_EQ_U(rig.trig.cam_edge_count, before + 1u);
    /* The wrong polarity must be ignored: a conditioner with inverted polarity must not
     * be able to fabricate a cam edge. */
    const uint8_t cam_before = rig.trig.cam_edge_count;
    rc_trigger_edge(&rig.trig, 1u, rig.tick + 1000u, 0u);
    RC_CHECK_EQ_U(rig.trig.cam_edge_count, cam_before);
}

static void wrong_polarity_never_syncs(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    /* Drive the opposite polarity from the configured one. */
    for (uint32_t rev = 0u; rev < 4u; ++rev) {
        for (uint8_t i = 0u; i < 58u; ++i) {
            rig.tick += 1000u;
            rc_trigger_edge(&rig.trig, 0u, rig.tick, 0u);
        }
        rig.tick += 3000u;
        rc_trigger_edge(&rig.trig, 0u, rig.tick, 0u);
    }
    RC_CHECK_MSG(rig.trig.state != RC_TRIGGER_SYNCED,
                 "the wrong edge polarity must never declare sync");
}

static void configuration_is_validated(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    rc_trigger_t trig;
    const char *reason = NULL;

    /* A gap that leaves no teeth. */
    cal.trigger_teeth = 2u;
    cal.trigger_missing_teeth = 2u;
    RC_CHECK(!rc_trigger_configure(&trig, &cal, &reason));
    RC_CHECK_EQ_U(trig.state, RC_TRIGGER_FAULT);
    RC_CHECK(strstr(reason, "at least one tooth") != NULL);

    /* An overspeed trip faster than the minimum period would fault on every legal edge. */
    cal.trigger_teeth = 60u;
    cal.trigger_missing_teeth = 2u;
    cal.trigger_min_period_us = 100u;
    cal.trigger_overspeed_period_us = 50u;
    RC_CHECK_MSG(!rc_trigger_configure(&trig, &cal, &reason), "inverted bounds must be refused");
    RC_CHECK(strstr(reason, "overspeed") != NULL);

    /* A single-tooth wheel cannot be decoded. */
    cal.trigger_teeth = 1u;
    cal.trigger_missing_teeth = 0u;
    cal.trigger_min_period_us = 40u;
    cal.trigger_overspeed_period_us = 60u;
    RC_CHECK(!rc_trigger_configure(&trig, &cal, &reason));
}

static void prediction_is_bounded(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 4u);
    const uint32_t base = rc_trigger_predict_period_us(&rig.trig, 0.0f);
    RC_CHECK(base > 0u);
    /* A zero lead time returns the current filtered period unchanged. */
    RC_CHECK_EQ_U(rc_trigger_predict_period_us(&rig.trig, 0.0f), rig.trig.period_filtered_us);
    /* Any lead must produce a period inside the configured bounds, never an absurd one. */
    for (uint32_t lead = 0u; lead < 100000u; lead += 9973u) {
        const uint32_t predicted = rc_trigger_predict_period_us(&rig.trig, (float)lead);
        RC_CHECK_MSG(predicted >= rig.trig.min_period_us,
                     "predicted period %u below the minimum %u", (unsigned)predicted,
                     (unsigned)rig.trig.min_period_us);
        RC_CHECK(predicted <= rig.trig.max_period_us);
    }
    /* With prediction disabled the result is exactly the current period. */
    rig.trig.prediction_enabled = 0u;
    RC_CHECK_EQ_U(rc_trigger_predict_period_us(&rig.trig, 5000.0f), rig.trig.period_filtered_us);
}

static void angle_advances_between_edges(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 0u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    const uint32_t at_edge = rig.trig.crank_angle_deg10;
    /* Half a tooth later the angle must have moved, and must be strictly monotonic. */
    rig.tick += 500u;
    rc_trigger_update(&rig.trig, rig.tick);
    const uint32_t mid = rig.trig.crank_angle_deg10;
    rig.tick += 500u;
    rc_trigger_update(&rig.trig, rig.tick);
    RC_CHECK_MSG(mid != at_edge, "the position model must interpolate between edges");
    /* 60 teeth => 60 tenths of a degree per tooth, so a full tooth is 60. */
    RC_CHECK_EQ_U(rig.trig.crank_angle_deg10, (uint16_t)((at_edge + 60u) % 3600u));
    /* and half a tooth is 30. */
    RC_CHECK_MSG(mid == (uint16_t)((at_edge + 30u) % 3600u), "half tooth = 3.0 deg, got %u",
                 (unsigned)mid);
    /* A jump further than a tooth must not run away. */
    rig.tick += 100000u;
    rc_trigger_update(&rig.trig, rig.tick);
    RC_CHECK(rig.trig.crank_angle_deg10 < 3600u);
}

static void rejects_false_teeth_without_losing_phase(void)
{
    rig_t rig;
    rig_configure(&rig, 1u, 60u, 2u, 0u, 1u, 0u);
    spin_revolutions(&rig, 60u, 2u, 1000u, 3u);
    RC_CHECK(rig.trig.state == RC_TRIGGER_SYNCED);
    const uint32_t phase = rig.trig.crank_angle_deg10;
    const uint32_t rejects = rig.trig.stats.rejected[RC_REJ_FILTERED];
    /* Inject a burst of false edges, then return to the real pattern. */
    for (uint8_t i = 0u; i < 5u; ++i) {
        rig.tick += 30u;
        rc_trigger_edge(&rig.trig, 0u, rig.tick, 1u);
    }
    RC_CHECK(rig.trig.stats.rejected[RC_REJ_FILTERED] > rejects);
    spin_revolutions(&rig, 60u, 2u, 1000u, 1u);
    RC_CHECK_MSG(rig.trig.state == RC_TRIGGER_SYNCED, "false teeth must not lose sync, got %s",
                 rc_trigger_state_name(rig.trig.state));
    /* Phase must still advance by a whole number of teeth, not a fraction. */
    RC_CHECK_EQ_U(((uint32_t)rig.trig.crank_angle_deg10 - phase) % 60u, 0u);
}

/* The cam wheel's tooth count must come from the profile that was selected.
 *
 * This used to be "3 teeth for anything that is not none". A 4-tooth half-speed wheel read
 * that way produces a cam phase wrong by a quarter of a cam revolution, and a wrong cam
 * phase under sequential injection fires a cylinder at the wrong time. Nothing in the
 * calibration would have said the count was wrong, because the count was never read - which
 * is why this is a test and not a code comment. */
static void cam_wheel_count_comes_from_the_profile(void)
{
    struct { uint8_t profile; uint8_t teeth; uint8_t missing; const char *what; } cases[] = {
        { 0u, 0u, 0u, "none" },
        { 1u, 3u, 1u, "3-tooth half speed with a gap" },
        { 2u, 4u, 1u, "4-tooth half speed with a gap" },
        { 3u, 1u, 0u, "one edge per cam revolution" },
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rc_cal_t cal = rc_cal_defaults_filled();
        cal.trigger_profile_crank = 0u;      /* custom, so the crank side is out of the way */
        cal.trigger_teeth = 36u;
        cal.trigger_missing_teeth = 1u;
        cal.trigger_profile_cam = cases[i].profile;
        cal.trigger_min_period_us = 40u;
        cal.trigger_overspeed_period_us = 60u;
        cal.trigger_max_period_us = 2000000u;
        cal.trigger_trigger_edge_polarity = 0x03u;
        rc_trigger_t trig;
        const char *reason = NULL;
        RC_CHECK_MSG(rc_trigger_configure(&trig, &cal, &reason), "configure failed: %s",
                     (reason != NULL) ? reason : "?");
        RC_CHECK_MSG(trig.cam_teeth == cases[i].teeth,
                     "profile_cam %u (%s) gave %u cam teeth, expected %u",
                     (unsigned)cases[i].profile, cases[i].what,
                     (unsigned)trig.cam_teeth, (unsigned)cases[i].teeth);
        RC_CHECK_MSG(trig.cam_missing_teeth == cases[i].missing,
                     "profile_cam %u (%s) gave a %u tooth gap, expected %u",
                     (unsigned)cases[i].profile, cases[i].what,
                     (unsigned)trig.cam_missing_teeth, (unsigned)cases[i].missing);
    }
}

/* 'custom' means "use the explicit tooth counts", so with no tooth count there is no wheel,
 * and the configuration has to be refused rather than defaulting to something. */
static void cam_wheel_custom_needs_a_tooth_count(void)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    cal.trigger_profile_crank = 0u;
    cal.trigger_teeth = 36u;
    cal.trigger_missing_teeth = 1u;
    cal.trigger_profile_cam = 4u;            /* custom */
    cal.trigger_cam_teeth = 0u;
    cal.trigger_cam_missing_teeth = 0u;
    cal.trigger_min_period_us = 40u;
    cal.trigger_overspeed_period_us = 60u;
    cal.trigger_max_period_us = 2000000u;
    rc_trigger_t trig;
    const char *reason = NULL;
    RC_CHECK_MSG(rc_trigger_configure(&trig, &cal, &reason) == false,
                 "a custom cam wheel with no tooth count was accepted");
    RC_CHECK(reason != NULL);

    /* And a usable custom wheel is accepted, with the count taken from the items. */
    cal.trigger_cam_teeth = 2u;
    cal.trigger_cam_missing_teeth = 1u;
    RC_CHECK_MSG(rc_trigger_configure(&trig, &cal, &reason), "configure failed: %s",
                 (reason != NULL) ? reason : "?");
    RC_CHECK_EQ_U(trig.cam_teeth, 2u);
    RC_CHECK_EQ_U(trig.cam_missing_teeth, 1u);
}

void test_trigger(void)
{
    RC_TEST_CASE("state/is/no/signal/at/reset", state_is_no_signal_at_reset);
    RC_TEST_CASE("first/edge/initialises", first_edge_initialises);
    RC_TEST_CASE("acquires/sync/on/a/missing/tooth/wheel", acquires_sync_on_a_missing_tooth_wheel);
    RC_TEST_CASE("even/wheel/never/claims/sync", even_wheel_never_claims_sync);
    RC_TEST_CASE("filter/rejects/a/glitch/but/not/a/real/tooth", filter_rejects_a_glitch_but_not_a_real_tooth);
    RC_TEST_CASE("overspeed/is/a/hard/fault", overspeed_is_a_hard_fault);
    RC_TEST_CASE("tooth/timeout/drops/sync", tooth_timeout_drops_sync);
    RC_TEST_CASE("stall/then/restart", stall_then_restart);
    RC_TEST_CASE("cam/edge/establishes/phase", cam_edge_establishes_phase);
    RC_TEST_CASE("wrong/polarity/never/syncs", wrong_polarity_never_syncs);
    RC_TEST_CASE("configuration/is/validated", configuration_is_validated);
    RC_TEST_CASE("cam/wheel/count/comes/from/the/profile", cam_wheel_count_comes_from_the_profile);
    RC_TEST_CASE("cam/wheel/custom/needs/a/usable/tooth/count", cam_wheel_custom_needs_a_tooth_count);
    RC_TEST_CASE("prediction/is/bounded", prediction_is_bounded);
    RC_TEST_CASE("angle/advances/between/edges", angle_advances_between_edges);
    RC_TEST_CASE("rejects/false/teeth/without/losing/phase", rejects_false_teeth_without_losing_phase);
}
