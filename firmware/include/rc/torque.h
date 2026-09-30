/* RACCOON V2 - torque manager.
 *
 * The single coordination point between everything that wants to change what the engine
 * does (spec section 40).  Nothing else in the firmware is allowed to modulate final fuel
 * or ignition authority.
 *
 * Rules, all of which are unit-tested and one of which is fuzzed (docs/TORQUE_CONTROL_SPEC.md):
 *   T1  Monotone de-authority.  A requester may only reduce authority.  Authority granted
 *       in one cycle cannot be spent in a later cycle without re-arbitration.
 *   T2  Fixed reduction priority.  When two requests disagree, the stronger reduction
 *       wins, by the table below - not by the order the functions happen to run in.
 *   T3  Cut ownership.  A cut belongs to exactly one requester and only that requester's
 *       re-enable predicate can clear it.  This is what makes "a cut decision is final"
 *       true in the code.
 *   T4  Hard limits are never lowered here.  Arbitration scales requests; the safety
 *       envelope runs afterwards and is not an input.
 *   T5  Latching requests are named, and each has a bounded lifetime.
 *   T6  Every arbitration is traceable, so a log can explain any single cylinder.
 */
#ifndef RC_TORQUE_H
#define RC_TORQUE_H

#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Requesters, ordered by reduction priority. Lower rank wins. The order IS the table. */
typedef enum {
    RC_REQ_NONE = 0,
    RC_REQ_SYNC_LOSS = 1,        /**< immediate, hardware-backed */
    RC_REQ_HARD_PROTECTION = 2,  /**< rpm, lean, oil, thermal, fuel pressure, battery */
    RC_REQ_KNOCK = 3,
    RC_REQ_LEAN = 4,
    RC_REQ_THERMAL = 5,
    RC_REQ_REV_LIMIT = 6,
    RC_REQ_TRACTION = 7,
    RC_REQ_SHIFT = 8,
    RC_REQ_LAUNCH = 9,
    RC_REQ_BOOST_PROTECTION = 10,
    RC_REQ_SENSOR_FAULT = 11,
    RC_REQ_ENGINE_BRAKING = 12,  /**< additive, the only one */
    RC_REQ_RIDER = 13,           /**< baseline */
    RC_REQ_COUNT = 14
} rc_requester_t;

/** A request: a requested scale, plus whether it latches. */
typedef struct {
    float scale;          /**< 0..1, 1 = no change requested */
    bool latching;        /**< needs a re-enable predicate, not just a timeout */
    bool engaged;
} rc_torque_request_t;

typedef struct {
    rc_torque_request_t request[RC_REQ_COUNT];
    /* Latch ownership, per requester: a latching request stays engaged until its owner
     * clears it. T3. */
    bool latched[RC_REQ_COUNT];
    uint32_t latch_tick_us[RC_REQ_COUNT];

    float allowed_scale;      /**< 0..1, the arbitration result */
    float retard_equivalent;  /**< degrees of retard implied by the lost authority */
    uint8_t winner;           /**< the requester that decided the result */
    uint32_t trace;           /**< bitmask of engaged requesters, for the log */
} rc_torque_manager_t;

/** Initialise: everything disengaged, full authority. */
void rc_torque_init(rc_torque_manager_t *tm);

/**
 * Submit a request for this cycle.
 *
 * @param scale         0..1
 * @param latching      true when the requester owns a cut that needs a predicate
 * @param tick_us       current engine time, used for latch expiry
 * @param latching_max_ms maximum latched lifetime, 0 = no expiry (only for requesters
 *                      whose own predicate is the exit, and the validator forbids a
 *                      latch without one)
 */
void rc_torque_request(rc_torque_manager_t *tm, rc_requester_t who, float scale,
                       bool latching, uint32_t tick_us, uint32_t latching_max_ms);

/**
 * Clear a requester's latch - ONLY from that requester's own re-enable predicate.
 * Any other caller is a bug, and the unit test for T3 proves the API makes it awkward.
 */
void rc_torque_release(rc_torque_manager_t *tm, rc_requester_t who);

/**
 * Arbitrate.
 *
 * @param retard_per_unit_loss degrees of retard equivalent to a complete loss of
 *        authority, so the ignition consequence of a torque reduction is a calibrated
 *        quantity rather than a guess.
 */
void rc_torque_arbitrate(rc_torque_manager_t *tm, float retard_per_unit_loss);

/** Clear every non-latching request. Called at the end of each control cycle (T1). */
void rc_torque_end_cycle(rc_torque_manager_t *tm);

/** True when the arbitration currently forbids fuel. */
bool rc_torque_fuel_forbidden(const rc_torque_manager_t *tm);

/** True when the arbitration currently forbids spark. */
bool rc_torque_spark_forbidden(const rc_torque_manager_t *tm);

/** The requester that owns the current cut, or RC_REQ_NONE. */
uint8_t rc_torque_cut_owner(const rc_torque_manager_t *tm);

/** Human-readable requester name. */
const char *rc_torque_requester_name(uint8_t who);

#ifdef __cplusplus
}
#endif
#endif /* RC_TORQUE_H */
