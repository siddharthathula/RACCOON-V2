/* RACCOON V2 - crank/cam trigger decoder and engine position model.
 *
 * The highest-risk subsystem in the product: everything downstream trusts the engine
 * angle this file produces.  It is therefore a pure state machine driven only by
 * timestamped edges, with no I/O of its own, so the whole thing - including every state
 * transition and every pathological edge stream - is exhaustively testable on the host.
 *
 * Invariants (docs/TRIGGER_SPEC.md):
 *   I1  Every state transition is reachable and testable; the test asserts the exact
 *       state sequence, not merely that it ends up somewhere plausible.
 *   I2  In every state other than SYNCED/DEGRADED, fuel and spark are inhibited.
 *   I3  Period bounds are the only source of "impossible period" and "overspeed".
 *   I4  A rejected edge never advances the phase model.
 *   I5  Acceleration is bounded; the predictor is disabled rather than extrapolated.
 *
 * DESIGN-NOTE
 *   Requirements: PRODUCT_REQUIREMENTS R-TRIG; docs/TRIGGER_SPEC.md.
 *   Public sources consulted: Speeduino's public wiki and doxygen for the *concept* of a
 *   decoder capability layer and a time-based input filter with named levels.  The state
 *   machine, the confidence model and the predictor are original.  No code was read or
 *   copied (THIRD_PARTY_LICENSE_REGISTER.md TPL-001).
 *   INDEPENDENTLY AUTHORED.
 */
#ifndef RC_TRIGGER_H
#define RC_TRIGGER_H

#include "cal_types.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Why an edge was rejected. Exposed so the trigger log can explain itself. */
typedef enum {
    RC_REJ_NONE = 0,
    RC_REJ_TOO_FAST = 1,       /**< below min_period_us */
    RC_REJ_OVERSPEED = 2,     /**< below the hard overspeed trip */
    RC_REJ_TOO_SLOW = 3,       /**< above max_period_us, or a tooth timeout */
    RC_REJ_FILTERED = 4,      /**< inside the software input filter window */
    RC_REJ_NO_SYNC = 5,       /**< no sync yet, edge not counted */
    RC_REJ_GAP_MISMATCH = 6   /**< a missing tooth gap did not match the profile */
} rc_reject_reason_t;

/** Counters. Every one is a logged channel; a tuner must be able to see the decoder's health. */
typedef struct {
    uint32_t edges_seen;
    uint32_t edges_accepted;
    uint32_t rejected[8];
    uint32_t sync_acquired;
    uint32_t sync_lost_events;
    uint32_t gap_events;
    uint32_t accel_out_of_bounds;
    uint32_t resync_count;
} rc_trigger_stats_t;

typedef struct {
    /* -- configuration, copied so the hot path never reads the calibration ------ */
    uint8_t teeth;            /**< teeth per crank revolution, gap included */
    uint8_t missing_teeth;    /**< size of the contiguous gap */
    uint8_t configured;       /**< set by rc_trigger_configure; NOT cleared by re-entry */
    uint8_t cam_teeth;        /**< 0 when no cam is fitted */
    uint8_t cam_missing_teeth; /**< gap on the cam wheel; 0 = no phase reference */
    uint8_t cam_polarity;     /**< 1 = active high */
    uint8_t crank_polarity;   /**< 1 = active high */
    uint8_t filter_level;     /**< 0..3 */
    uint8_t sync_skip_revolutions;
    uint32_t min_period_us;
    uint32_t max_period_us;
    uint32_t overspeed_period_us;
    uint8_t prediction_enabled;
    uint32_t max_accel_rpm_per_s;
    uint16_t angle_per_tooth_x1000; /**< crank degrees per tooth, x1000, wrap-safe aware */

    /* -- state --------------------------------------------------------------- */
    uint8_t state;
    uint8_t previous_state;
    uint8_t sync_confidence;
    uint8_t tooth_index;
    uint8_t in_gap;                /**< 1 while inside the missing-tooth gap */
    uint8_t cam_seen_this_cycle;
    uint8_t cam_edge_count;        /**< increments per cam revolution */
    uint16_t cycle_count;
    uint16_t revs_since_candidate;
    uint16_t candidate_count;
    uint32_t candidate_tooth;

    uint32_t last_tick_us;
    /** Consecutive edges shorter than min_period.  One short edge is a glitch; a run of
     * them is a genuinely fast signal, which is what makes the overspeed trip real rather
     * than a single noisy sample latching the engine off. */
    uint8_t consecutive_fast;
    uint32_t tooth_period_us;
    uint32_t period_filtered_us;
    uint32_t last_gap_period_us;
    int32_t accel_rpm_per_s;   /**< filtered d(rpm)/dt; signed and bounded */
    /** The crank angle AT the most recent accepted edge. The live angle is this plus the
     * time elapsed since that edge, so calling rc_trigger_update repeatedly is idempotent
     * and cannot run the position model forward. */
    uint16_t angle_at_edge_deg10;
    uint16_t crank_angle_deg10;
    uint16_t cycle_angle_deg10;
    uint8_t cylinder_phase;

    rc_trigger_stats_t stats;
} rc_trigger_t;

/**
 * Configure the decoder from calibration.
 *
 * The function returns false, leaving the decoder in RC_TRIGGER_FAULT, when the trigger
 * configuration is not self-consistent - for example a gap that leaves no teeth, or an
 * overspeed trip faster than the minimum plausible period.  The same checks exist at
 * build time (rc_rules.py overspeed_period_above_min) and at activation
 * (cal_validate.c); this is the third, cheap, local gate.
 */
bool rc_trigger_configure(rc_trigger_t *trig, const rc_cal_t *cal, const char **reason);

/** Reset to NO_SIGNAL, clearing statistics. */
void rc_trigger_reset(rc_trigger_t *trig);

/**
 * Feed one position-sensor edge.
 *
 * @param trig      decoder state
 * @param channel   0 = crank, 1 = cam
 * @param tick_us   capture timestamp
 * @param level     sampled level, already conditioned to a clean square wave
 *
 * Must be O(1) and allocation-free: this runs in the crank interrupt.
 */
void rc_trigger_edge(rc_trigger_t *trig, uint8_t channel, uint32_t tick_us, uint8_t level);

/**
 * Advance the position model to `now` without an edge.
 *
 * Interpolates the crank angle inside the current tooth.  Called by the control cycle
 * so that a log or a CAN frame taken between edges still carries a meaningful angle.
 */
void rc_trigger_update(rc_trigger_t *trig, uint32_t now_us);

/** The current position model, for publication in the engine state. */
void rc_trigger_get_position(const rc_trigger_t *trig, rc_engine_position_t *out);

/** Drop sync if no edge has arrived for longer than the timeout permits.
 *
 *  The too-slow check inside the edge handler cannot catch this, because it only runs when
 *  an edge DOES arrive.  A trigger wire that is cut, a sensor that dies, or a crank that
 *  simply stops turning produce no edges at all, and without a watchdog the decoder would
 *  report SYNCED - and full control - indefinitely, on a position it stopped believing
 *  seconds ago.  Call this from the control cycle.
 *
 *  @return true if sync was lost because of the timeout. */
bool rc_trigger_check_timeout(rc_trigger_t *trig, uint32_t now_us);

/** Period the decoder *predicts* at the target angle, bounded by max_accel_rpm_per_s. */
uint32_t rc_trigger_predict_period_us(rc_trigger_t *trig, float lead_us);

/** True when the state permits fuel and spark. */
bool rc_trigger_permits_control(const rc_trigger_t *trig);

/** Human-readable state name, for the tuner and the logs. */
const char *rc_trigger_state_name(uint8_t state);

/** Stringify a rejection reason. */
const char *rc_trigger_reject_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
/** Consecutive too-fast edges needed before the hard overspeed trip latches. */
#define RC_TRIGGER_FAST_RUN_TO_FAULT 3u

#endif /* RC_TRIGGER_H */
