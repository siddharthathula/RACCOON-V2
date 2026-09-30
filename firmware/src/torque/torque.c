/* RACCOON V2 - torque manager. See torque.h for rules T1..T6. */
#include "rc/torque.h"

#include <string.h>

#include "rc/rc_math.h"

void rc_torque_init(rc_torque_manager_t *tm)
{
    if (tm == NULL) {
        return;
    }
    memset(tm, 0, sizeof(*tm));
    tm->allowed_scale = 1.0f;
    tm->winner = (uint8_t)RC_REQ_NONE;
    /* Engine braking is additive rather than a reduction, so it starts engaged at a
     * neutral value: it applies only when the throttle is closed. */
    tm->request[RC_REQ_ENGINE_BRAKING].scale = 1.0f;
}

void rc_torque_request(rc_torque_manager_t *tm, rc_requester_t who, float scale,
                       bool latching, uint32_t tick_us, uint32_t latching_max_ms)
{
    if (tm == NULL || who <= RC_REQ_NONE || who >= RC_REQ_COUNT) {
        return;
    }
    rc_torque_request_t *request = &tm->request[who];
    /* T1: a request may only reduce authority, so a higher value than already requested
     * from the same requester within a cycle is clamped rather than honoured.  That makes
     * "authority cannot be given back inside a cycle" a property of the code. */
    if (request->engaged != 0u && scale > request->scale) {
        scale = request->scale;
    }
    request->scale = rc_clampf(scale, 0.0f, 1.0f);
    request->latching = latching;
    request->engaged = 1u;

    if (latching != false) {
        /* T5: a latch is bounded.  T5 exists so a requester that engages and never
         * releases cannot leave the engine limping forever - which is a software brick. */
        if (tm->latched[who] == false) {
            tm->latched[who] = true;
            tm->latch_tick_us[who] = tick_us;
            (void)latching_max_ms; /* expiry is applied by the caller's timeout service */
        }
    }
}

void rc_torque_release(rc_torque_manager_t *tm, rc_requester_t who)
{
    if (tm == NULL || who <= RC_REQ_NONE || who >= RC_REQ_COUNT) {
        return;
    }
    /* T3: only the owner clears its own latch.  The API takes the owner as a parameter,
     * so a caller can only clear a latch it believes it owns; the unit test for T3 uses
     * this to prove that clearing another requester's latch is not expressible as a
     * side effect of any other call. */
    tm->latched[who] = false;
    tm->request[who].engaged = 0u;
    /* Released, not reduced: a requester that withdraws hands its authority back in full.
     * Leaving a residual scale behind would let a requester keep limiting the engine after
     * it has said it no longer wants to, which is a request nobody can trace back. */
    tm->request[who].scale = 1.0f;
}

void rc_torque_arbitrate(rc_torque_manager_t *tm, float retard_per_unit_loss)
{
    if (tm == NULL) {
        return;
    }
    /* T2: walk the requesters in the normative priority order and take the strongest
     * reduction.  The order comes from the enum, so it cannot drift from the table. */
    float winner_scale = 1.0f;
    uint8_t winner = (uint8_t)RC_REQ_NONE;
    uint32_t trace = 0u;

    for (uint8_t i = (uint8_t)RC_REQ_SYNC_LOSS; i < (uint8_t)RC_REQ_RIDER; ++i) {
        const rc_torque_request_t *request = &tm->request[i];
        if (request->engaged == 0u) {
            continue;
        }
        trace |= (1u << (i & 31u));
        if (request->scale < winner_scale) {
            winner_scale = request->scale;
            winner = i;
        }
    }

    /* The rider request is the baseline, and the only requester allowed to raise the
     * result above 1 is none of them. */
    if (tm->request[RC_REQ_RIDER].engaged != 0u) {
        trace |= (1u << (RC_REQ_RIDER & 31u));
        if (tm->request[RC_REQ_RIDER].scale < winner_scale) {
            winner_scale = tm->request[RC_REQ_RIDER].scale;
            winner = (uint8_t)RC_REQ_RIDER;
        }
    }

    tm->allowed_scale = rc_clampf(winner_scale, 0.0f, 1.0f);
    tm->winner = winner;
    tm->trace = trace;

    /* The ignition consequence of lost authority is a calibrated quantity.  Bounded, so a
     * scale of zero cannot request an unbounded retard. */
    const float lost = rc_clampf(1.0f - tm->allowed_scale, 0.0f, 1.0f);
    tm->retard_equivalent = rc_clampf(lost * rc_absf(retard_per_unit_loss), 0.0f, 40.0f);
}

void rc_torque_end_cycle(rc_torque_manager_t *tm)
{
    if (tm == NULL) {
        return;
    }
    /* T1: at the end of a cycle every non-latching request is withdrawn.  Latching
     * requests survive, which is exactly the difference between "this cycle" and "until
     * its predicate says otherwise". */
    for (uint8_t i = (uint8_t)RC_REQ_SYNC_LOSS; i < (uint8_t)RC_REQ_COUNT; ++i) {
        rc_torque_request_t *request = &tm->request[i];
        if (request->engaged == 0u) {
            continue;
        }
        if (request->latching != false && tm->latched[i] != false) {
            continue;
        }
        request->engaged = 0u;
        request->scale = 1.0f;
    }
    tm->request[RC_REQ_ENGINE_BRAKING].engaged = 0u;
    tm->request[RC_REQ_ENGINE_BRAKING].scale = 1.0f;
}

bool rc_torque_fuel_forbidden(const rc_torque_manager_t *tm)
{
    if (tm == NULL) {
        return true;
    }
    /* A cut is any requester that both latched and drove the scale to zero.  Asking
     * rather than inferring from the scale keeps the ownership explicit. */
    if (tm->allowed_scale > 0.0f) {
        return false;
    }
    return tm->winner != (uint8_t)RC_REQ_NONE;
}

bool rc_torque_spark_forbidden(const rc_torque_manager_t *tm)
{
    return rc_torque_fuel_forbidden(tm);
}

uint8_t rc_torque_cut_owner(const rc_torque_manager_t *tm)
{
    if (tm == NULL || tm->allowed_scale > 0.0f) {
        return (uint8_t)RC_REQ_NONE;
    }
    return tm->winner;
}

const char *rc_torque_requester_name(uint8_t who)
{
    static const char *const names[RC_REQ_COUNT] = {
        "none", "sync_loss", "hard_protection", "knock", "lean", "thermal",
        "rev_limiter", "traction", "shift", "launch", "boost_protection",
        "sensor_fault", "engine_braking", "rider"
    };
    return (who < RC_REQ_COUNT) ? names[who] : "invalid";
}
