/* RACCOON V2 - crank-angle event scheduler.
 *
 * The scheduler is the only thing that turns a *decision* into an *edge*.  Its
 * invariants are the reason fuel and spark can be trusted:
 *
 *   S1  No resurrection.  An event that reached FIRED or CANCELLED at generation g can
 *       never execute again; only a strictly newer generation may replace it.
 *   S2  No duplicates.  An identical (type, cylinder, generation) is refused.
 *   S3  Stale rejection.  An event older than the live plan generation is discarded,
 *       never executed late.
 *   S4  Explicit expiry.  An event not executed by its deadline becomes EXPIRED, is
 *       counted, and raises a defined safety action - never a silent late pulse.
 *   S5  Wrap-safe time.  All comparisons use unsigned wrap-safe differences; a deadline
 *       that appears "in the past" after a 32-bit wrap is correctly in the future.
 *   S6  No channel collision.  Two live events for the same channel are a configuration
 *       error; at runtime the lower-priority event is dropped and counted.
 *
 * The CPU only ever loads a timer compare register (rc_hal_arm_event).  The timer, not
 * the firmware, produces the edge, which is what makes the timing bound a hardware
 * property (docs/FIRMWARE_ARCHITECTURE.md section 4).
 *
 * DESIGN-NOTE
 *   Requirements: PRODUCT_REQUIREMENTS R-SCHED; docs spec section 13.
 *   Public sources consulted: rusEFI's public HTML architecture documentation for the
 *   "compute once per revolution, schedule from the nearest trigger event" model and for
 *   the idea of angle->time offsets.  The queue, the generation guard and the expiry
 *   policy are original.  No code read or copied (TPL-002).
 *   INDEPENDENTLY AUTHORED.
 */
#ifndef RC_SCHEDULER_H
#define RC_SCHEDULER_H

#include "rc/rc_hal.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Counters, all published as logged channels. */
typedef struct {
    uint32_t scheduled;
    uint32_t refused_duplicate;
    uint32_t refused_stale;
    uint32_t refused_collision;
    uint32_t refused_full;
    uint32_t expired;
    uint32_t fired;
    uint32_t cancelled;
    uint32_t max_lateness_us;
} rc_scheduler_stats_t;

typedef struct {
    rc_event_t queue[RC_SCHEDULER_QUEUE];
    uint16_t count;
    /** The live plan generation. Everything older is stale (S3). */
    uint16_t plan_generation;
    /** Safety escalation requested by expiry, cleared by the caller after handling. */
    uint32_t expiry_mask; /**< bit i set = expiry on event type i */
    rc_scheduler_stats_t stats;
} rc_scheduler_t;

/** Reset to an empty queue at generation 0. */
void rc_sched_init(rc_scheduler_t *sched);

/**
 * Begin a new plan.
 *
 * Every subsequent rc_sched_schedule() belongs to this generation.  Bumping the
 * generation invalidates anything still pending from the previous plan, which is how a
 * mode change or a strategy change takes effect without a late event from the old
 * strategy executing (S1, S3).
 */
void rc_sched_new_plan(rc_scheduler_t *sched);

/** The live plan generation. */
uint16_t rc_sched_generation(const rc_scheduler_t *sched);

/**
 * Request an event.
 *
 * @return RC_HAL_OK, or a negative error: RC_HAL_ERR_PARAM for a malformed request,
 *         RC_HAL_ERR_REFUSED for a duplicate, stale or colliding event.
 *
 * A target tick already in the past is accepted and fires on the next dispatch, which is
 * how "this cycle is late" degrades rather than wrapping a second ahead.
 */
int rc_sched_schedule(rc_scheduler_t *sched, rc_event_type_t type, uint8_t cylinder,
                      uint32_t target_tick_us, uint32_t deadline_us, uint8_t priority);

/** Convenience: schedule an angle offset from `from_us` at the predicted period. */
int rc_sched_schedule_angle(rc_scheduler_t *sched, rc_event_type_t type, uint8_t cylinder,
                            uint32_t from_us, float degrees, uint32_t period_us,
                            uint32_t deadline_us, uint8_t priority);

/**
 * Cancel every pending event of a type, optionally only for one cylinder.
 *
 * Used by fuel cut and ignition cut.  Cancellation is one-way: a cancelled event stays
 * cancelled, which is what makes "a cut decision is final" true in the code rather than
 * only in the documentation (spec section 27).
 */
uint32_t rc_sched_cancel(rc_scheduler_t *sched, rc_event_type_t type, int cylinder);

/**
 * Dispatch every due event.
 *
 * @param now_us current tick
 * @param hal    hardware interface; may be NULL in a pure-queue test, in which case
 *               events are marked fired but no hardware is touched
 * @return the number of events executed
 *
 * Also expires anything past its deadline, per S4.
 */
uint32_t rc_sched_dispatch(rc_scheduler_t *sched, uint32_t now_us, rc_hal_t *hal,
                               void *hal_ctx);

/** Force every output to its hardware-safe state, whatever the queue believes. */
void rc_sched_force_safe(const rc_scheduler_t *sched, rc_hal_t *hal, void *hal_ctx);

/** Clear the expiry escalation mask after the caller has acted on it. */
void rc_sched_clear_expiry(rc_scheduler_t *sched);

/** Number of pending events, for the ECU health page. */
uint16_t rc_sched_pending(const rc_scheduler_t *sched);

#ifdef __cplusplus
}
#endif
#endif /* RC_SCHEDULER_H */
