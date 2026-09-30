/* RACCOON V2 - virtual hardware back end.
 *
 * Logical time, synthesised position-sensor edges, captured actuator edges.  This is what
 * the unit tests, the SIL and the crank simulator run against, so they exercise the
 * shipping firmware rather than a model of it (docs/TESTING_ARCHITECTURE.md section 1).
 *
 * It is *not* a mock: every function is a faithful implementation of the HAL contract, and
 * the only thing that differs from silicon is where the time comes from.
 */
#include "rc/hal_virtual.h"

#include <string.h>

void rc_hal_virtual_init(rc_hal_virtual_t *hal)
{
    if (hal == NULL) {
        return;
    }
    memset(hal, 0, sizeof(*hal));
    hal->base.tick_now_us = rc_hal_virtual_tick;
    hal->base.arm_event = rc_hal_virtual_arm_event;
    hal->base.disarm_event = rc_hal_virtual_disarm_event;
    hal->base.force_safe_output = rc_hal_virtual_force_safe_output;
    hal->base.capture_enable = rc_hal_virtual_capture_enable;
    hal->base.capture_disable = rc_hal_virtual_capture_disable;
    hal->base.adc_start = rc_hal_virtual_adc_start;
    hal->base.adc_stop = rc_hal_virtual_adc_stop;
    hal->base.digital_read = rc_hal_virtual_digital_read;
    hal->base.bus_send = rc_hal_virtual_bus_send;
    hal->base.bus_receive_enable = rc_hal_virtual_bus_receive_enable;
    hal->base.storage_read = rc_hal_virtual_storage_read;
    hal->base.storage_write = rc_hal_virtual_storage_write;
    hal->base.cycle_count = rc_hal_virtual_cycle_count;
    hal->base.reset_reason = rc_hal_virtual_reset_reason;
    hal->storage_size = sizeof(hal->storage);
    hal->reset = RC_RESET_POWER_ON;
}

uint32_t rc_hal_virtual_tick(void *user)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    return (hal != NULL) ? hal->now_us : 0u;
}

int rc_hal_virtual_arm_event(void *user, const rc_event_t *event)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || event == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    /* A timer compare register is one-shot and holds one compare value per channel:
     * re-arming the same channel for the same plan generation is what real hardware
     * silently does, and pretending otherwise would hide a scheduler bug. */
    for (uint32_t i = 0u; i < hal->armed_count; ++i) {
        if (hal->armed[i].event.type == event->type
            && hal->armed[i].event.cylinder == event->cylinder
            && hal->armed[i].event.generation == event->generation) {
            hal->arm_refused++;
            return RC_HAL_ERR_REFUSED;
        }
    }
    for (uint32_t i = 0u; i < hal->delivery_count; ++i) {
        const rc_event_t *done = &hal->deliveries[i % RC_HAL_VIRTUAL_MAX_DELIVERED];
        if (done->type == event->type && done->cylinder == event->cylinder
            && done->generation == event->generation) {
            hal->arm_refused++;
            return RC_HAL_ERR_REFUSED;
        }
    }
    if (hal->armed_count >= RC_HAL_VIRTUAL_MAX_ARMED) {
        hal->arm_refused++;
        return RC_HAL_ERR_BUSY;
    }
    /* Deliver the edge immediately if it is already due; otherwise hold it.  This mirrors
     * a timer compare: the edge lands at the compare, not when the CPU looks. */
    rc_virtual_armed_t *slot = &hal->armed[hal->armed_count++];
    slot->event = *event;
    slot->armed_at_us = event->target_tick;
    slot->delivered = false;
    if ((int32_t)(event->target_tick - hal->now_us) <= 0) {
        hal->deliveries[hal->delivery_count % RC_HAL_VIRTUAL_MAX_DELIVERED] = *event;
        hal->delivery_count++;
        slot->delivered = true;
        hal->armed_count--;
    }
    return RC_HAL_OK;
}

void rc_hal_virtual_disarm_event(void *user, uint8_t type, uint8_t cylinder, uint16_t generation)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL) {
        return;
    }
    uint16_t kept = 0u;
    for (uint16_t i = 0u; i < hal->armed_count; ++i) {
        const rc_virtual_armed_t *slot = &hal->armed[i];
        if (slot->event.type == type && slot->event.cylinder == cylinder
            && slot->event.generation <= generation) {
            hal->disarms++;
            continue;
        }
        hal->armed[kept++] = *slot;
    }
    hal->armed_count = kept;
}

void rc_hal_virtual_force_safe_output(void *user, uint8_t type, uint8_t cylinder)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL) {
        return;
    }
    hal->forced_safe++;
    hal->safe[0][cylinder % RC_MAX_CYLINDERS] = 1u;
    (void)type;
}

int rc_hal_virtual_capture_enable(void *user, uint8_t channel, rc_capture_cb_t cb, void *cb_user)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || channel >= 2u) {
        return RC_HAL_ERR_PARAM;
    }
    hal->capture_cb[channel] = cb;
    hal->capture_user[channel] = cb_user;
    return RC_HAL_OK;
}

void rc_hal_virtual_capture_disable(void *user, uint8_t channel)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal != NULL && channel < 2u) {
        hal->capture_cb[channel] = NULL;
    }
}

int rc_hal_virtual_adc_start(void *user, uint32_t rate_hz, rc_adc_cb_t cb, void *cb_user)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    hal->adc_cb = cb;
    hal->adc_user = cb_user;
    hal->adc_rate_hz = rate_hz;
    return RC_HAL_OK;
}

void rc_hal_virtual_adc_stop(void *user)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal != NULL) {
        hal->adc_cb = NULL;
    }
}

int rc_hal_virtual_digital_read(void *user, uint8_t channel, bool *level)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || level == NULL || channel >= RC_HAL_VIRTUAL_DIGITALS) {
        return RC_HAL_ERR_PARAM;
    }
    *level = hal->digital[channel] != 0u;
    return RC_HAL_OK;
}

int rc_hal_virtual_bus_send(void *user, uint8_t port, const rc_bus_frame_t *frame)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || frame == NULL || port >= 2u) {
        return RC_HAL_ERR_PARAM;
    }
    if (hal->tx_count >= RC_HAL_VIRTUAL_TX_CAPACITY) {
        hal->tx_overflow++;
        return RC_HAL_ERR_BUSY;
    }
    hal->tx[hal->tx_count++] = *frame;
    return RC_HAL_OK;
}

int rc_hal_virtual_bus_receive_enable(void *user, uint8_t port, rc_bus_cb_t cb, void *cb_user)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || port >= 2u) {
        return RC_HAL_ERR_PARAM;
    }
    hal->rx_cb[port] = cb;
    hal->rx_user[port] = cb_user;
    return RC_HAL_OK;
}

/* The `user` parameter is the HAL's `void *ctx` slot and cannot be const: the function is
 * installed into rc_hal_t, and making the parameter const would require casting the
 * function pointer to match.  The local is const instead, so the callback still cannot
 * write through it. */
/* cppcheck-suppress constParameterCallback */
int rc_hal_virtual_storage_read(void *user, uint32_t offset, void *dst, uint32_t len)
{
    const rc_hal_virtual_t *hal = (const rc_hal_virtual_t *)user;
    if (hal == NULL || dst == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    if (offset + len > hal->storage_size) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(dst, &hal->storage[offset], len);
    return RC_HAL_OK;
}

int rc_hal_virtual_storage_write(void *user, uint32_t offset, const void *src, uint32_t len)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (hal == NULL || src == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    if (offset + len > hal->storage_size) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(&hal->storage[offset], src, len);
    hal->storage_writes++;
    return RC_HAL_OK;
}

/* Every accessor here only reads the handle, and says so with `const`. The cast from the
 * HAL's `void *` is unavoidable: that is the interface. */
// cppcheck-suppress constParameterCallback
uint32_t rc_hal_virtual_cycle_count(void *user)
{
    const rc_hal_virtual_t *hal = (const rc_hal_virtual_t *)user;
    return (hal != NULL) ? hal->now_us / 3u : 0u; /* the target runs the timer at 3x the
                                                   * nominal 1 MHz tick clock */
}

void rc_hal_virtual_reset_reason(void *user, uint32_t *reason)
{
    rc_hal_virtual_t *hal = (rc_hal_virtual_t *)user;
    if (reason == NULL) {
        return;
    }
    *reason = (hal != NULL) ? hal->reset : RC_RESET_UNKNOWN;
}

/* --------------------------------------------------------------- stimulus */

void rc_hal_virtual_set_time(rc_hal_virtual_t *hal, uint32_t now_us)
{
    if (hal == NULL) {
        return;
    }
    hal->now_us = now_us;
}

void rc_hal_virtual_advance(rc_hal_virtual_t *hal, uint32_t delta_us)
{
    if (hal == NULL) {
        return;
    }
    hal->now_us += delta_us;
}

void rc_hal_virtual_pulse(rc_hal_virtual_t *hal, uint8_t channel, uint32_t tick_us, uint8_t level)
{
    if (hal == NULL || channel >= 2u) {
        return;
    }
    rc_capture_edge_t edge;
    edge.tick_us = tick_us;
    edge.channel = channel;
    edge.level = level;
    hal->edges++;
    if (hal->capture_cb[channel] != NULL) {
        hal->capture_cb[channel](&edge, hal->capture_user[channel]);
    }
}

void rc_hal_virtual_publish_adc(rc_hal_virtual_t *hal, uint32_t tick_us, uint32_t sequence,
                                const uint16_t *raw, uint8_t count)
{
    if (hal == NULL || hal->adc_cb == NULL) {
        return;
    }
    rc_adc_frame_t frame;
    frame.tick_us = tick_us;
    frame.sequence = sequence;
    frame.count = (count > RC_MAX_SENSORS) ? (uint8_t)RC_MAX_SENSORS : count;
    for (uint8_t i = 0u; i < frame.count; ++i) {
        frame.raw[i] = raw[i];
    }
    hal->adc_frames++;
    hal->adc_cb(&frame, hal->adc_user);
}

void rc_hal_virtual_pump_events(rc_hal_virtual_t *hal)
{
    if (hal == NULL) {
        return;
    }
    /* Fire every armed slot whose compare time has arrived, and retire it: a timer compare
     * is a one-shot.  Iterating in index order keeps delivery deterministic. */
    uint16_t kept = 0u;
    for (uint16_t i = 0u; i < hal->armed_count; ++i) {
        rc_virtual_armed_t *slot = &hal->armed[i];
        if ((int32_t)(slot->armed_at_us - hal->now_us) <= 0) {
            hal->deliveries[hal->delivery_count % RC_HAL_VIRTUAL_MAX_DELIVERED] = slot->event;
            hal->delivery_count++;
            /* Retiring a compare raises an interrupt, and that interrupt is what tells the
             * scheduler the edge happened.  Without this the scheduler is left believing
             * the event is still outstanding and re-arms it. */
            if (hal->base.on_event_delivered != NULL) {
                hal->base.on_event_delivered(hal->base.owner, &slot->event);
            }
            continue;
        }
        hal->armed[kept++] = *slot;
    }
    hal->armed_count = kept;
}
