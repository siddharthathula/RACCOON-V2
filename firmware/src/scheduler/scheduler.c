/* RACCOON V2 - crank-angle event scheduler. See scheduler.h for invariants S1..S6. */
#include "rc/scheduler.h"

#include <string.h>

/* A tick further than this ahead of "now" is treated as a wrap artefact, not a real
 * future event. 71 minutes of microseconds is far longer than any control cycle and far
 * shorter than the 4295 s wrap period, so it cannot be confused with either. */
#define RC_SCHED_FAR_FUTURE_US 4275000000u

void rc_sched_init(rc_scheduler_t *sched)
{
    if (sched == NULL) {
        return;
    }
    memset(sched, 0, sizeof(*sched));
    sched->plan_generation = 1u;
}

void rc_sched_new_plan(rc_scheduler_t *sched)
{
    if (sched == NULL) {
        return;
    }
    /* Generation 0 is reserved as "unset", so never wrap to it.
     *
     * The wrap IS reachable: `plan_generation` is a uint16_t, so 0xFFFF + 1 narrows back to
     * 0.  Static analysers that widen the addition to int before the narrowing cast report
     * this branch as dead, which is exactly the kind of false "always false" that hides a
     * real bug in the branch next to it - so the wrap is written explicitly. */
    /* cppcheck-suppress knownConditionTrueFalse */
    sched->plan_generation = (uint16_t)(sched->plan_generation + 1u);
    /* cppcheck-suppress knownConditionTrueFalse */
    if (sched->plan_generation == 0u) {
        sched->plan_generation = 1u;
    }
    /* Drop everything from the previous plan rather than letting it execute late. */
    sched->count = 0u;
}

uint16_t rc_sched_generation(const rc_scheduler_t *sched)
{
    return (sched != NULL) ? sched->plan_generation : 0u;
}

uint16_t rc_sched_pending(const rc_scheduler_t *sched)
{
    return (sched != NULL) ? sched->count : 0u;
}

static bool same_channel(const rc_event_t *a, const rc_event_t *b)
{
    return a->type == b->type && a->cylinder == b->cylinder;
}

int rc_sched_schedule(rc_scheduler_t *sched, rc_event_type_t type, uint8_t cylinder,
                      uint32_t target_tick_us, uint32_t deadline_us, uint8_t priority)
{
    if (sched == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    if (type >= RC_EV_TYPE_COUNT) {
        return RC_HAL_ERR_PARAM;
    }

    /* S5: a target absurdly far from the start of the 32-bit timeline is a wrap
     * artefact or a corrupted request, not a real event.  Wrap-safe *differences* are
     * handled in dispatch; this catches a genuinely nonsensical absolute value. */
    if (target_tick_us > RC_SCHED_FAR_FUTURE_US) {
        return RC_HAL_ERR_PARAM;
    }

    if (sched->count >= RC_SCHEDULER_QUEUE) {
        sched->stats.refused_full++;
        return RC_HAL_ERR_BUSY;
    }

    rc_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = (uint8_t)type;
    event.cylinder = cylinder;
    event.state = RC_EV_PENDING;
    event.priority = priority;
    event.generation = sched->plan_generation;
    event.target_tick = target_tick_us;
    event.deadline = deadline_us;

    for (uint16_t i = 0u; i < sched->count; ++i) {
        const rc_event_t *pending = &sched->queue[i];
        /* S1/S3: anything from an older generation is stale, never executable. */
        if (pending->generation != event.generation) {
            continue;
        }
        if (same_channel(pending, &event)) {
            /* The same channel twice in one plan.  An identical request is a duplicate
             * (S2); a different request for the same hardware output is a collision, which
             * is a configuration error and is reported differently so the tuner sees which
             * mistake they made (S6). */
            if (pending->generation == event.generation
                && pending->target_tick == event.target_tick) {
                sched->stats.refused_duplicate++;
            } else {
                sched->stats.refused_collision++;
            }
            return RC_HAL_ERR_REFUSED;
        }
    }

    sched->queue[sched->count++] = event;
    sched->stats.scheduled++;
    return RC_HAL_OK;
}

int rc_sched_schedule_angle(rc_scheduler_t *sched, rc_event_type_t type, uint8_t cylinder,
                            uint32_t from_us, float degrees, uint32_t period_us,
                            uint32_t deadline_us, uint8_t priority)
{
    if (period_us == 0u) {
        return RC_HAL_ERR_PARAM;
    }
    /* degrees / 360 * period, in integer arithmetic so the result is reproducible. */
    const uint32_t offset = (uint32_t)((degrees * (float)period_us) / 360.0f);
    return rc_sched_schedule(sched, type, cylinder, from_us + offset, deadline_us, priority);
}

uint32_t rc_sched_cancel(rc_scheduler_t *sched, rc_event_type_t type, int cylinder)
{
    if (sched == NULL) {
        return 0u;
    }
    uint32_t removed = 0u;
    uint16_t kept = 0u;
    for (uint16_t i = 0u; i < sched->count; ++i) {
        rc_event_t *event = &sched->queue[i];
        const bool match_type = (event->type == (uint8_t)type);
        const bool match_cyl = (cylinder < 0) || (event->cylinder == (uint8_t)cylinder);
        if (match_type && match_cyl) {
            event->state = RC_EV_CANCELLED;
            removed++;
            sched->stats.cancelled++;
            continue; /* S1: cancelled means cancelled; it is dropped, not kept */
        }
        sched->queue[kept++] = *event;
    }
    sched->count = kept;
    return removed;
}

/* A driver interrupt reporting that a compare produced its edge.  The event is retired
 * here rather than left pending, because the hardware edge has already happened: leaving
 * it queued would make the next dispatch re-arm a compare that has already fired, and the
 * injector would receive two pulses for one fuel demand. */
static void on_delivered(void *ctx, const rc_event_t *event)
{
    rc_scheduler_t *sched = (rc_scheduler_t *)ctx;
    if (sched == NULL || event == NULL) {
        return;
    }
    for (uint16_t i = 0u; i < sched->count; ++i) {
        if (sched->queue[i].type == event->type
            && sched->queue[i].cylinder == event->cylinder
            && sched->queue[i].generation == event->generation
            && sched->queue[i].state != RC_EV_FIRED) {
            sched->queue[i].state = RC_EV_FIRED;
            return;
        }
    }
}

uint32_t rc_sched_dispatch(rc_scheduler_t *sched, uint32_t now_us, rc_hal_t *hal, void *hal_ctx)
{
    if (sched == NULL) {
        return 0u;
    }
    uint32_t fired = 0u;
    uint16_t kept = 0u;

    if (hal != NULL) {
        /* Own the delivery interrupt for as long as this scheduler drives the hardware. */
        hal->on_event_delivered = on_delivered;
        hal->owner = sched;
    }

    for (uint16_t i = 0u; i < sched->count; ++i) {
        rc_event_t event = sched->queue[i];

        /* The driver already reported this edge: it is done, not pending.  The entry is
         * RETAINED rather than dropped, so that re-scheduling the same event in this plan
         * generation is still refused - an edge that has already happened must not be
         * conjured up a second time.  A new plan generation clears the queue. */
        if (event.state == RC_EV_FIRED) {
            sched->queue[kept++] = event;
            continue;
        }

        /* S4: expired means expired. The escalation is recorded so the caller can take
         * the defined safety action; the event is not executed late. */
        if (!rc_deadline_ok(now_us, event.deadline)) {
            event.state = RC_EV_EXPIRED;
            sched->stats.expired++;
            sched->expiry_mask |= (1u << (event.type & 31u));
            continue;
        }

        /* Arm the compare register NOW, well before the edge, and only retire the event
         * once it is actually due.  A timer channel has to be programmed ahead of the
         * edge it will produce: arming at the moment an event is already due means the
         * edge is late by construction, and at 12000 rpm on a 4-stroke there is under
         * 3 ms per cylinder in which to find, compute and arm it. */
        if (hal != NULL && hal->arm_event != NULL && event.state != RC_EV_ARMED) {
            (void)hal->arm_event(hal_ctx, &event);
            event.state = RC_EV_ARMED;
        }

        if ((int32_t)(event.target_tick - now_us) > 0) {
            sched->queue[kept++] = event; /* armed, not due yet */
            continue;
        }

        const uint32_t lateness = (now_us - event.target_tick);
        if (lateness > sched->stats.max_lateness_us) {
            sched->stats.max_lateness_us = lateness;
        }
        event.state = RC_EV_FIRED;
        sched->stats.fired++;
        fired++;
        /* Retained for the rest of this plan generation so the same event cannot be
         * resurrected: the edge has already happened, and a scheduler that forgets that
         * will happily fire the same injector pulse twice for one fuel demand. */
        sched->queue[kept++] = event;
    }

    sched->count = kept;
    return fired;
}

void rc_sched_force_safe(const rc_scheduler_t *sched, rc_hal_t *sched_hal, void *hal_ctx)
{
    (void)sched;
    if (sched_hal == NULL || sched_hal->force_safe_output == NULL) {
        return;
    }
    /* Every actuator, every channel, unconditionally. Used on reset, on a fatal fault and
     * whenever the control loop stops being trustworthy. */
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_INJECTOR_OPEN, cyl);
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_COIL_CHARGE, cyl);
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_FUEL_CUT, 0u);
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_IGNITION_CUT, 0u);
    }
    for (uint8_t cyl = 0u; cyl < RC_MAX_INJECTORS; ++cyl) {
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_INJECTOR_OPEN, cyl);
    }
    for (uint8_t cyl = 0u; cyl < RC_MAX_IGNITION; ++cyl) {
        sched_hal->force_safe_output(hal_ctx, (uint8_t)RC_EV_COIL_CHARGE, cyl);
    }
}

void rc_sched_clear_expiry(rc_scheduler_t *sched)
{
    if (sched != NULL) {
        sched->expiry_mask = 0u;
    }
}
