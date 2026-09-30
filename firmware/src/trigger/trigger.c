/* RACCOON V2 - trigger decoder. See trigger.h for the invariants and design note. */
#include "rc/trigger.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"

/* ------------------------------------------------------------------ helpers */

static void set_state(rc_trigger_t *trig, uint8_t next)
{
    if (trig->state == next) {
        return;
    }
    trig->previous_state = trig->state;
    trig->state = next;
    switch (next) {
        case RC_TRIGGER_NO_SIGNAL:
        case RC_TRIGGER_INITIALIZING:
        case RC_TRIGGER_SEEKING_SYNC:
            trig->sync_confidence = 0u;
            break;
        case RC_TRIGGER_SYNC_CANDIDATE:
            trig->sync_confidence = 40u;
            break;
        case RC_TRIGGER_SYNCED:
            trig->sync_confidence = 100u;
            trig->stats.sync_acquired++;
            break;
        case RC_TRIGGER_DEGRADED:
            /* Confidence decays rather than collapsing, so a single noisy edge does not
             * look like a lost sync. */
            if (trig->sync_confidence > 60u) {
                trig->sync_confidence = 60u;
            }
            break;
        case RC_TRIGGER_SYNC_LOST:
            trig->sync_confidence = 0u;
            trig->stats.sync_lost_events++;
            break;
        case RC_TRIGGER_RECOVERING:
            trig->sync_confidence = 10u;
            trig->stats.resync_count++;
            break;
        case RC_TRIGGER_FAULT:
            trig->sync_confidence = 0u;
            break;
        default:
            break;
    }
}

/** Tooth count for the configured wheel, at least 1 so division is safe. */
static uint32_t teeth_of(const rc_trigger_t *trig)
{
    return (trig->teeth > 0u) ? trig->teeth : 1u;
}

/** Number of tooth-periods the missing-tooth gap spans.
 *
 *  Measured between the last real tooth before the gap and the first real tooth after it,
 *  that interval is `missing + 1` tooth periods wide: a 60-2 wheel has 58 teeth and one
 *  gap three periods long, not two.  Getting this wrong scales the measured period, and
 *  therefore the reported rpm, by the ratio of the two counts. */
static uint32_t gap_intervals_of(const rc_trigger_t *trig)
{
    return (trig->missing_teeth + 1u);
}

static void reject(rc_trigger_t *trig, uint8_t reason)
{
    const uint8_t slot = (reason < 8u) ? reason : 7u;
    /* edges_seen is already counted once per physical edge in on_crank_edge, so it is not
     * touched here: counting it twice would make the seen/accepted ratio meaningless. */
    trig->stats.rejected[slot]++;
}

/** Engine rpm implied by a per-tooth period.
 *
 *  A tooth period is NOT a revolution.  The engine completes one revolution per `teeth`
 *  teeth, so the revolution rate is 60e6 / (teeth * period) rpm.  Reporting the raw tooth
 *  rate here would inflate rpm by the tooth count - a 60-tooth wheel would read 60x its
 *  true speed, which would trip every overspeed and rev-limit in the engine at idle. */
static float rpm_from_period_us(const rc_trigger_t *trig)
{
    const uint32_t period = (trig->period_filtered_us != 0u) ? trig->period_filtered_us
                                                            : trig->tooth_period_us;
    if (period == 0u) {
        return 0.0f;
    }
    const uint32_t teeth = (trig->teeth > 0u) ? trig->teeth : 1u;
    return 60000000.0f / ((float)teeth * (float)period);
}

/* ----------------------------------------------------------------- configure */

bool rc_trigger_configure(rc_trigger_t *trig, const rc_cal_t *cal, const char **reason)
{
    const char *why = NULL;
    if (trig == NULL || cal == NULL) {
        if (reason != NULL) {
            *reason = "null argument";
        }
        return false;
    }
    memset(trig, 0, sizeof(*trig));

    uint8_t teeth = cal->trigger_teeth;
    uint8_t missing = cal->trigger_missing_teeth;
    const uint8_t profile = cal->trigger_profile_crank;

    /* Named generic profiles override the explicit tooth counts, so a tuner does not
     * have to know that a 36-1 wheel has 36 teeth and a 1 tooth gap. */
    switch (profile) {
        case 1: teeth = 60u; missing = 2u; break;  /* mt_60_2 */
        case 2: teeth = 60u; missing = 3u; break;  /* mt_60_3 */
        case 3: teeth = 36u; missing = 1u; break;  /* mt_36_1 */
        case 4: teeth = 36u; missing = 2u; break;  /* mt_36_2 */
        case 5: teeth = 36u; missing = 3u; break;  /* mt_36_3 */
        case 6: teeth = 24u; missing = 1u; break;  /* mt_24_1 */
        case 7: teeth = 24u; missing = 2u; break;  /* mt_24_2 */
        case 8: teeth = 16u; missing = 1u; break;  /* mt_16_1 */
        case 9: teeth = 24u; missing = 0u; break;  /* even_24 */
        case 10: teeth = 36u; missing = 0u; break; /* even_36 */
        case 11: teeth = 60u; missing = 0u; break; /* even_60 */
        default: break;                            /* custom and dual/quadrature */
    }

    if (teeth < 2u) {
        why = "trigger needs at least 2 teeth per revolution";
        goto fail;
    }
    if (missing >= teeth) {
        why = "the missing-tooth gap must leave at least one tooth";
        goto fail;
    }
    if (cal->trigger_teeth_per_rev != 1u && cal->trigger_teeth_per_rev != 2u) {
        why = "teeth_per_rev must be 1 (crank) or 2 (cam)";
        goto fail;
    }

    const uint32_t min_period = cal->trigger_min_period_us;
    const uint32_t max_period = cal->trigger_max_period_us;
    const uint32_t overspeed = cal->trigger_overspeed_period_us;
    if (min_period >= max_period) {
        why = "min_period_us must be below max_period_us";
        goto fail;
    }
    if (overspeed <= min_period) {
        why = "overspeed_period_us must be above min_period_us, otherwise every legal "
              "edge would be declared an overspeed fault";
        goto fail;
    }

    trig->teeth = teeth;
    trig->missing_teeth = missing;

    /* The cam wheel, from the selected profile.
     *
     * This used to be "3 teeth for anything that is not none", which silently mis-decodes
     * every cam wheel that is not 3-tooth. A 4-tooth half-speed wheel read as 3 teeth gives
     * a cam phase wrong by a quarter of a cam revolution, and a wrong cam phase under
     * sequential injection fires a cylinder at the wrong time. That is a broken engine, not
     * a tune that needs work - and nothing in the calibration said the count was wrong,
     * because the count was never read. */
    uint8_t cam_teeth = 0u;
    uint8_t cam_missing = 0u;
    switch (cal->trigger_profile_cam) {
        case 1: cam_teeth = 3u; cam_missing = 1u; break;  /* mt_3_1 */
        case 2: cam_teeth = 4u; cam_missing = 1u; break;  /* mt_4_1 */
        case 3: cam_teeth = 1u; cam_missing = 0u; break;  /* single edge per cam rev */
        case 4:                                        /* custom */
            cam_teeth = cal->trigger_cam_teeth;
            cam_missing = cal->trigger_cam_missing_teeth;
            if (cam_teeth < 2u) {
                why = "profile_cam is 'custom' but cam_teeth is "
                      "fewer than 2, so there is no cam wheel to decode";
                goto fail;
            }
            if (cam_missing >= cam_teeth) {
                why = "cam_missing_teeth must leave at least one cam tooth";
                goto fail;
            }
            break;
        default: break;                                  /* none */
    }
    trig->cam_teeth = cam_teeth;
    trig->cam_missing_teeth = cam_missing;
    trig->crank_polarity = (uint8_t)((cal->trigger_trigger_edge_polarity & 0x01u) ? 1u : 0u);
    trig->cam_polarity = (uint8_t)((cal->trigger_trigger_edge_polarity & 0x02u) ? 1u : 0u);
    trig->filter_level = cal->trigger_input_filter_level;
    trig->sync_skip_revolutions = cal->trigger_sync_skip_revolutions;
    trig->min_period_us = min_period;
    trig->max_period_us = max_period;
    trig->overspeed_period_us = overspeed;
    trig->prediction_enabled = cal->trigger_prediction_enabled;
    trig->max_accel_rpm_per_s = cal->trigger_max_accel_rpm_per_s;
    /* Crank degrees per tooth, in 0.1 degree units, so the angle stays integral. */
    trig->angle_per_tooth_x1000 = (uint16_t)(3600u / (uint32_t)teeth);

    set_state(trig, RC_TRIGGER_NO_SIGNAL);
    if (reason != NULL) {
        *reason = NULL;
    }
        trig->configured = 1u;
return true;

fail:
    memset(trig, 0, sizeof(*trig));
    trig->state = RC_TRIGGER_FAULT;
    if (reason != NULL) {
        *reason = (why != NULL) ? why : "invalid trigger configuration";
    }
    return false;
}

void rc_trigger_reset(rc_trigger_t *trig)
{
    if (trig == NULL) {
        return;
    }
    const rc_trigger_stats_t stats = trig->stats;
    const uint8_t teeth = trig->teeth;
    const uint8_t missing = trig->missing_teeth;
    const uint8_t cam = trig->cam_teeth;
    const uint16_t apt = trig->angle_per_tooth_x1000;
    const uint8_t cpol = trig->crank_polarity;
    const uint8_t mpol = trig->cam_polarity;
    const uint8_t filt = trig->filter_level;
    const uint8_t skip = trig->sync_skip_revolutions;
    const uint32_t minp = trig->min_period_us;
    const uint32_t maxp = trig->max_period_us;
    const uint32_t over = trig->overspeed_period_us;
    const uint8_t pred = trig->prediction_enabled;
    const uint32_t acc = trig->max_accel_rpm_per_s;

    memset(trig, 0, sizeof(*trig));
    trig->teeth = teeth;
    trig->missing_teeth = missing;
    trig->cam_teeth = cam;
    trig->crank_polarity = cpol;
    trig->cam_polarity = mpol;
    trig->filter_level = filt;
    trig->sync_skip_revolutions = skip;
    trig->min_period_us = minp;
    trig->max_period_us = maxp;
    trig->overspeed_period_us = over;
    trig->prediction_enabled = pred;
    trig->max_accel_rpm_per_s = acc;
    trig->angle_per_tooth_x1000 = apt;
    trig->stats = stats;
    set_state(trig, RC_TRIGGER_NO_SIGNAL);
}

/* --------------------------------------------------------------------- edges */

static void advance_phase(rc_trigger_t *trig, uint8_t teeth_passed)
{
    for (uint8_t i = 0u; i < teeth_passed; ++i) {
        trig->tooth_index = (uint8_t)((trig->tooth_index + 1u) % trig->teeth);
        if (trig->tooth_index == 0u) {
            trig->revs_since_candidate++;
            if (trig->cam_teeth == 0u || trig->cam_seen_this_cycle) {
                trig->cycle_count++;
                trig->cam_seen_this_cycle = 0u;
            }
        }
    }
    uint32_t angle = ((uint32_t)trig->tooth_index * (uint32_t)trig->angle_per_tooth_x1000) % 3600u;
    if (trig->in_gap != 0u) {
        /* Inside the gap the crank is still turning; the model holds the end-of-gap angle
         * so scheduling inside the gap interpolates rather than stalling. */
        angle = ((uint32_t)trig->angle_per_tooth_x1000
                 * (uint32_t)(trig->teeth - trig->missing_teeth)) % 3600u;
    }
    trig->crank_angle_deg10 = (uint16_t)angle;
    trig->cycle_angle_deg10 = (uint16_t)(((uint32_t)trig->cycle_angle_deg10
                                          + (uint32_t)teeth_passed * trig->angle_per_tooth_x1000)
                                         % 7200u);
}

static void update_accel(rc_trigger_t *trig, uint32_t period_us)
{
    if (trig->period_filtered_us == 0u) {
        trig->period_filtered_us = period_us;
        trig->accel_rpm_per_s = 0;
        return;
    }
    /* Acceleration from the change in period over the elapsed time.  Bounded: a
     * physically impossible change disables the predictor rather than extrapolating. */
    const int32_t delta = (int32_t)trig->period_filtered_us - (int32_t)(uint32_t)period_us;
    /* The zero case returned above, so this divisor is known non-zero here. */
    const int32_t divisor = (int32_t)trig->period_filtered_us;
    const int32_t accel = (delta * 1000) / divisor;
    const int32_t limit_accel = (int32_t)trig->max_accel_rpm_per_s;
    int32_t smoothed = trig->accel_rpm_per_s + ((accel - trig->accel_rpm_per_s) / 8);
    if (smoothed > limit_accel) {
        smoothed = limit_accel;
    }
    if (smoothed < -limit_accel) {
        smoothed = -limit_accel;
    }
    trig->accel_rpm_per_s = smoothed;
    /* Light IIR on the period: constant-Q behaviour near steady state, which is what
     * spark scheduling wants, unlike a plain moving average. */
    trig->period_filtered_us = ((((uint32_t)trig->period_filtered_us * 3u)
                                 + (uint32_t)period_us) / 4u);
}

static void on_crank_edge(rc_trigger_t *trig, uint32_t tick_us, uint8_t level)
{
    const uint8_t active = trig->crank_polarity;
    /* Only the configured edge polarity advances the decoder: a conditioner may emit on
     * either edge, and treating both as position is how phantom sync is born. */
    if ((level == active) == false) {
        return;
    }

    trig->stats.edges_seen++;

    if (trig->last_tick_us == 0u) {
        /* First edge: there is no period yet, only a place to start. */
        trig->last_tick_us = tick_us;
        trig->tooth_period_us = 0u;
        set_state(trig, RC_TRIGGER_INITIALIZING);
        return;
    }

    const uint32_t period = rc_ticks_forward(tick_us, trig->last_tick_us);

    /* ---- I3: period bounds are the only source of impossible/overspeed -------
     * The software filter runs FIRST, and that ordering is the whole point: a single
     * implausibly short edge is noise, and a noise-driven overspeed trip would shut the
     * engine off on one bad reading.  Only a *run* of short edges is treated as a real
     * fast signal, and only that run trips the hard fault. */
    if (trig->filter_level > 0u && trig->period_filtered_us > 0u) {
        /* Window scales with RPM: a fixed time filter either passes noise at speed or
         * rejects real teeth at low speed. Percentages are 20/28/35 % of a period. */
        static const uint16_t window_pct[4] = {0u, 20u, 28u, 35u};
        const uint32_t window = (trig->period_filtered_us * window_pct[trig->filter_level]) / 100u;
        if (window > 0u && period < window) {
            reject(trig, RC_REJ_FILTERED);
            return;
        }
    }
    if (period < trig->min_period_us) {
        /* With the filter switched off the operator has asked to trust the signal, so a
         * single too-fast edge is taken at face value and trips immediately.  With the
         * filter on, the run length is what separates a glitch from a fast signal. */
        const uint8_t run_needed = (trig->filter_level > 0u) ? RC_TRIGGER_FAST_RUN_TO_FAULT : 1u;
        if (++trig->consecutive_fast >= run_needed) {
            reject(trig, RC_REJ_OVERSPEED);
            set_state(trig, RC_TRIGGER_FAULT);
        } else {
            reject(trig, RC_REJ_TOO_FAST);
        }
        return;
    }
    trig->consecutive_fast = 0u;
    if (period < trig->overspeed_period_us) {
        reject(trig, RC_REJ_OVERSPEED);
        set_state(trig, RC_TRIGGER_FAULT);
        return;
    }
    /* "Too slow" is judged against the raw configured limit, NOT scaled by the gap.
     * max_period_us is the operator's "the signal is gone" threshold: it is already
     * generous enough to cover a legitimate gap at cranking rpm.  Scaling it by the gap
     * width would quietly multiply the real loss threshold by three, and a silent engine
     * would keep reporting the rpm it had when it stopped.  If a legitimate gap can exceed
     * the limit at low rpm, the correct fix is to raise max_period_us, not to inflate it
     * here - because the same inflation would also stretch genuine loss detection. */
    if (period > trig->max_period_us) {
        reject(trig, RC_REJ_TOO_SLOW);
        set_state(trig, RC_TRIGGER_NO_SIGNAL);
        trig->last_tick_us = tick_us;
        /* Clear the speed estimate: a decoder that has lost the signal must report zero
         * rpm, not the rpm it happened to have when the signal died.  A stale rpm is what
         * makes a rev limiter hold, a shift light stay on, and launch logic engage. */
        trig->period_filtered_us = 0u;
        trig->tooth_period_us = 0u;
        trig->accel_rpm_per_s = 0;
        return;
    }

    const uint32_t prev_period = trig->tooth_period_us;
    trig->last_tick_us = tick_us;
    trig->stats.edges_accepted++;
    /* The decoder is at a tooth boundary: record the angle it landed on.  The live angle
     * is derived from this and the elapsed time, so repeated update() calls are
     * idempotent instead of each one adding a whole interval again. */
    /* The angle at a tooth boundary IS the tooth's phase: tooth 0 at TDC, each subsequent
     * tooth one tooth-angle later.  On a 60-tooth wheel that is 6 degrees per tooth. */
    trig->angle_at_edge_deg10 =
        (uint16_t)(((uint32_t)trig->tooth_index * 3600u) / teeth_of(trig));

    /* ---- gap handling ------------------------------------------------------ */
    const uint8_t has_gap = (trig->missing_teeth > 0u) ? 1u : 0u;
    if (has_gap == 0u) {
        /* An even wheel has no gap and therefore no phase reference at all.  RPM is valid
         * and the tooth count is unambiguous, but WHICH cylinder is at TDC is not, so the
         * decoder stays in SEEKING_SYNC forever.  Claiming sync here would let the engine
         * run with an arbitrary ignition phase, which is how a 4-cylinder ends up with two
         * cylinders firing into each other's compression. */
        trig->tooth_period_us = period;
        update_accel(trig, period);
        advance_phase(trig, 1u);
        trig->in_gap = 0u;
        if (trig->state == RC_TRIGGER_NO_SIGNAL || trig->state == RC_TRIGGER_INITIALIZING) {
            set_state(trig, RC_TRIGGER_SEEKING_SYNC);
        }
        return;
    }

    /* A period noticeably longer than the running average is the missing-tooth gap.  The
     * gap is the ONLY phase reference, so it must fall through to the state machine below
     * rather than returning early - otherwise sync can never be acquired. */
    const bool gap_now = (prev_period > 0u) ? (period > (prev_period * 3u) / 2u) : false;
    if (gap_now) {
        /* The gap interval is `missing + 1` tooth periods wide, so it must be divided
         * before it is allowed near the period average - otherwise the average inflates
         * and the reported rpm falls by that factor. */
        const uint32_t tooth_period = period / gap_intervals_of(trig);
        trig->tooth_period_us = tooth_period;
        update_accel(trig, tooth_period);
        trig->in_gap = 1u;
        trig->last_gap_period_us = period;
        trig->stats.gap_events++;
        advance_phase(trig, trig->missing_teeth);
        trig->tooth_index = 0u;
        trig->in_gap = 0u;
    } else {
        trig->tooth_period_us = period;
        update_accel(trig, period);
        advance_phase(trig, 1u);
        /* An ordinary tooth cannot establish a phase reference, so it only moves the
         * decoder along the search path. */
        if (trig->state == RC_TRIGGER_NO_SIGNAL || trig->state == RC_TRIGGER_INITIALIZING) {
            set_state(trig, RC_TRIGGER_SEEKING_SYNC);
        }
        return;
    }

    /* ---- sync acquisition: only a gap gets here ---------------------------- */
    if (trig->state == RC_TRIGGER_SEEKING_SYNC) {
        trig->candidate_tooth = trig->tooth_index;
        trig->candidate_count = 1u;
        set_state(trig, RC_TRIGGER_SYNC_CANDIDATE);
        return;
    }
    if (trig->state == RC_TRIGGER_NO_SIGNAL || trig->state == RC_TRIGGER_INITIALIZING) {
        set_state(trig, RC_TRIGGER_SEEKING_SYNC);
        return;
    }
    if (trig->state == RC_TRIGGER_SYNC_CANDIDATE) {
        const uint16_t required = (uint16_t)(1u + trig->sync_skip_revolutions);
        if (gap_now && trig->candidate_tooth == trig->tooth_index) {
            trig->candidate_count++;
            if (trig->candidate_count >= required) {
                set_state(trig, RC_TRIGGER_SYNCED);
            }
        } else {
            set_state(trig, RC_TRIGGER_SEEKING_SYNC);
            trig->candidate_count = 0u;
        }
        return;
    }
    if (trig->state == RC_TRIGGER_SYNCED) {
        if (gap_now) {
            /* The gap re-established the reference: full confidence. */
            set_state(trig, RC_TRIGGER_SYNCED);
        }
        return;
    }
    if (trig->state == RC_TRIGGER_DEGRADED) {
        /* One clean gap is enough to climb back out of DEGRADED. */
        if (gap_now) {
            set_state(trig, RC_TRIGGER_SYNCED);
        } else if (trig->sync_confidence < 100u) {
            trig->sync_confidence++;
        }
        return;
    }
    if (trig->state == RC_TRIGGER_RECOVERING) {
        if (gap_now) {
            set_state(trig, RC_TRIGGER_SYNC_CANDIDATE);
        }
        return;
    }
}

static void on_cam_edge(rc_trigger_t *trig, uint32_t tick_us, uint8_t level)
{
    if (trig->cam_teeth == 0u) {
        return;
    }
    if ((level == trig->cam_polarity) == false) {
        return;
    }
    (void)tick_us;
    trig->cam_seen_this_cycle = 1u;
    trig->cam_edge_count++;
    if (trig->state == RC_TRIGGER_RECOVERING) {
        set_state(trig, RC_TRIGGER_SYNC_CANDIDATE);
    }
}

void rc_trigger_edge(rc_trigger_t *trig, uint8_t channel, uint32_t tick_us, uint8_t level)
{
    if (trig == NULL || trig->state == RC_TRIGGER_FAULT) {
        return;
    }
    if (channel == 0u) {
        on_crank_edge(trig, tick_us, level);
    } else {
        on_cam_edge(trig, tick_us, level);
    }
}

/* ------------------------------------------------------------------ position */

bool rc_trigger_check_timeout(rc_trigger_t *trig, uint32_t now_us)
{
    if (trig == NULL) {
        return false;
    }
    if (trig->state == RC_TRIGGER_NO_SIGNAL || trig->state == RC_TRIGGER_FAULT) {
        return false;
    }
    /* Two full tooth periods with nothing at all is not a measurement, it is a dead
     * signal.  Two rather than one so a single missed tooth - a filter rejecting noise, a
     * slow ADC - cannot be mistaken for a lost trigger. */
    const uint32_t period = (trig->tooth_period_us != 0u) ? trig->tooth_period_us : 0u;
    if (period == 0u) {
        return false;
    }
    const uint32_t timeout = period * 2u * (uint32_t)teeth_of(trig);
    if (timeout == 0u || rc_ticks_forward(now_us, trig->last_tick_us) <= timeout) {
        return false;
    }
    reject(trig, RC_REJ_TOO_SLOW);
    set_state(trig, RC_TRIGGER_NO_SIGNAL);
    /* The speed estimate dies with the signal: reporting the last known rpm for a dead
     * trigger is how a rev limiter stays on and a shift light stays lit. */
    trig->period_filtered_us = 0u;
    trig->tooth_period_us = 0u;
    trig->accel_rpm_per_s = 0;
    return true;
}

void rc_trigger_update(rc_trigger_t *trig, uint32_t now_us)
{
    if (trig == NULL) {
        return;
    }
    if (trig->last_tick_us == 0u || trig->tooth_period_us == 0u) {
        trig->crank_angle_deg10 = 0u;
        return;
    }
    uint32_t elapsed = rc_ticks_forward(now_us, trig->last_tick_us);
    if (elapsed > trig->tooth_period_us) {
        /* More than a full tooth has passed without an edge: the signal is gone, but the
         * caller sees that through the state machine and its timeout, not from here. */
        elapsed = trig->tooth_period_us;
    }
    /* A tooth is one revolution's worth of angle divided by the tooth count: 360/teeth
     * degrees, which is `elapsed / (tooth_period * teeth) * 3600` in tenths of a degree.
     *
     * Dividing by the tooth period alone treats a single tooth as if it were a whole
     * revolution, which overstates the angle by the tooth count - on a 60-tooth wheel the
     * model would run 60x too fast, so every ignition edge would be commanded at a wildly
     * wrong crank angle while the decoder still reported SYNCED.
     *
     * The result is computed from the angle at the last edge plus the time since it, NOT
     * accumulated onto the previous reading: this function is called on a timer, so
     * accumulating would advance the position by one whole interval per call. */
    /* Both divisors are non-zero: the tooth period was checked above and teeth_of() never
     * returns zero.  A divide-by-zero here would be a hard fault, so the invariant is
     * stated by relying on the guard rather than by a redundant second check. */
    const uint32_t divisor = trig->tooth_period_us;
    const uint32_t teeth = teeth_of(trig);
    const uint32_t advance = (uint32_t)(((uint64_t)elapsed * 3600ull) / (divisor * teeth));
    trig->crank_angle_deg10 = (uint16_t)(((uint32_t)trig->angle_at_edge_deg10 + advance) % 3600u);
    trig->cycle_angle_deg10 =
        (uint16_t)(((uint32_t)trig->cycle_angle_deg10 + advance) % 7200u);
}

void rc_trigger_get_position(const rc_trigger_t *trig, rc_engine_position_t *out)
{
    if (trig == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const float rpm = rpm_from_period_us(trig);
    if (rpm > 200000.0f) {
        out->rpm_x2 = 400000u; /* clamp to a sane ceiling; the overspeed trip fires first */
    } else {
        out->rpm_x2 = (uint32_t)(rpm * 2.0f);
    }
    out->crank_angle_deg10 = trig->crank_angle_deg10;
    out->cycle_angle_deg10 = trig->cycle_angle_deg10;
    out->tooth_index = trig->tooth_index;
    out->tooth_period_us = trig->tooth_period_us;
    out->period_filtered_us = trig->period_filtered_us;
    out->sync_state = trig->state;
    out->sync_confidence = trig->sync_confidence;
    out->cylinder_phase = trig->cylinder_phase;
    out->accel_rpm_per_s = trig->accel_rpm_per_s;
    out->decel_rpm_per_s = (trig->accel_rpm_per_s < 0) ? -trig->accel_rpm_per_s : 0;
}

uint32_t rc_trigger_predict_period_us(rc_trigger_t *trig, float lead_us)
{
    if (trig == NULL || trig->period_filtered_us == 0u) {
        return 0u;
    }
    if (trig->prediction_enabled == 0u || lead_us <= 0.0f) {
        return trig->period_filtered_us;
    }
    /* rpm rises => period falls. dPeriod = -Period^2 * dRpm / (60 * rpm^2), or equivalently
     * the relative period change equals the relative rpm change over the lead time. */
    const float period = (float)trig->period_filtered_us;
    const float rpm = (period > 0.0f) ? (60000000.0f / period) : 0.0f;
    if (rpm <= 0.0f) {
        return trig->period_filtered_us;
    }
    const float lead_s = lead_us * 1.0e-6f;
    const float predicted_rpm = rpm + (float)trig->accel_rpm_per_s * lead_s;
    if (predicted_rpm <= 1.0f) {
        return 0xFFFFFFFFu; /* the engine cannot slow below 1 rpm; saturate high */
    }
    float predicted_period = 60000000.0f / predicted_rpm;
    if (predicted_period < (float)trig->min_period_us) {
        /* Out of bounds: clamp, and count it, rather than extrapolating further. */
        predicted_period = (float)trig->min_period_us;
        trig->stats.accel_out_of_bounds++;
    }
    if (predicted_period > (float)trig->max_period_us) {
        predicted_period = (float)trig->max_period_us;
    }
    return (uint32_t)predicted_period;
}

bool rc_trigger_permits_control(const rc_trigger_t *trig)
{
    return trig != NULL && rc_trigger_allows_control((rc_trigger_state_t)trig->state);
}

const char *rc_trigger_state_name(uint8_t state)
{
    switch (state) {
        case RC_TRIGGER_NO_SIGNAL: return "NO_SIGNAL";
        case RC_TRIGGER_INITIALIZING: return "INITIALIZING";
        case RC_TRIGGER_SEEKING_SYNC: return "SEEKING_SYNC";
        case RC_TRIGGER_SYNC_CANDIDATE: return "SYNC_CANDIDATE";
        case RC_TRIGGER_SYNCED: return "SYNCED";
        case RC_TRIGGER_DEGRADED: return "DEGRADED";
        case RC_TRIGGER_SYNC_LOST: return "SYNC_LOST";
        case RC_TRIGGER_RECOVERING: return "RECOVERING";
        case RC_TRIGGER_FAULT: return "FAULT";
        default: return "INVALID";
    }
}

const char *rc_trigger_reject_name(uint8_t reason)
{
    switch (reason) {
        case RC_REJ_NONE: return "none";
        case RC_REJ_TOO_FAST: return "too_fast";
        case RC_REJ_OVERSPEED: return "overspeed";
        case RC_REJ_TOO_SLOW: return "too_slow";
        case RC_REJ_FILTERED: return "filtered";
        case RC_REJ_NO_SYNC: return "no_sync";
        case RC_REJ_GAP_MISMATCH: return "gap_mismatch";
        default: return "invalid";
    }
}
