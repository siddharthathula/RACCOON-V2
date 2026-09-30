/* RACCOON V2 - virtual hardware back end. See hal_virtual.c. */
#ifndef RC_HAL_VIRTUAL_H
#define RC_HAL_VIRTUAL_H

#include "rc/rc_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_HAL_VIRTUAL_MAX_ARMED 64u
#define RC_HAL_VIRTUAL_MAX_DELIVERED 256u
#define RC_HAL_VIRTUAL_TX_CAPACITY 128u
#define RC_HAL_VIRTUAL_DIGITALS 16u
#define RC_HAL_VIRTUAL_STORAGE 4096u

typedef struct {
    rc_event_t event;
    uint32_t armed_at_us;
    bool delivered;
} rc_virtual_armed_t;

typedef struct {
    rc_hal_t base;

    uint32_t now_us;
    uint32_t reset;

    rc_capture_cb_t capture_cb[2];
    void *capture_user[2];
    rc_adc_cb_t adc_cb;
    void *adc_user;
    uint32_t adc_rate_hz;
    rc_bus_cb_t rx_cb[2];
    void *rx_user[2];

    rc_virtual_armed_t armed[RC_HAL_VIRTUAL_MAX_ARMED];
    uint16_t armed_count;
    rc_event_t deliveries[RC_HAL_VIRTUAL_MAX_DELIVERED];
    uint32_t delivery_count;
    uint32_t arm_refused;
    uint32_t disarms;
    uint32_t forced_safe;
    uint8_t safe[2][RC_MAX_CYLINDERS];

    uint8_t digital[RC_HAL_VIRTUAL_DIGITALS];

    rc_bus_frame_t tx[RC_HAL_VIRTUAL_TX_CAPACITY];
    uint32_t tx_count;
    uint32_t tx_overflow;

    uint8_t storage[RC_HAL_VIRTUAL_STORAGE];
    uint32_t storage_size;
    uint32_t storage_writes;

    uint32_t edges;
    uint32_t adc_frames;
} rc_hal_virtual_t;

/** Fill a virtual HAL with a valid function table and a zeroed state. */
void rc_hal_virtual_init(rc_hal_virtual_t *hal);

/* --- stimulus, used by tests, the SIL and the crank simulator ---------------- */

/** Set logical time directly. */
void rc_hal_virtual_set_time(rc_hal_virtual_t *hal, uint32_t now_us);
/** Advance logical time. */
void rc_hal_virtual_advance(rc_hal_virtual_t *hal, uint32_t delta_us);
/** Deliver one position-sensor edge through the registered capture callback. */
void rc_hal_virtual_pulse(rc_hal_virtual_t *hal, uint8_t channel, uint32_t tick_us, uint8_t level);
/** Publish one analog frame through the registered ADC callback. */
void rc_hal_virtual_publish_adc(rc_hal_virtual_t *hal, uint32_t tick_us, uint32_t sequence,
                                const uint16_t *raw, uint8_t count);
/** Deliver every armed event whose compare time has arrived. */
void rc_hal_virtual_pump_events(rc_hal_virtual_t *hal);

/* --- the HAL entry points, exposed so they can be called directly in tests ---- */

uint32_t rc_hal_virtual_tick(void *user);
int rc_hal_virtual_arm_event(void *user, const rc_event_t *event);
void rc_hal_virtual_disarm_event(void *user, uint8_t type, uint8_t cylinder, uint16_t generation);
void rc_hal_virtual_force_safe_output(void *user, uint8_t type, uint8_t cylinder);
int rc_hal_virtual_capture_enable(void *user, uint8_t channel, rc_capture_cb_t cb, void *cb_user);
void rc_hal_virtual_capture_disable(void *user, uint8_t channel);
int rc_hal_virtual_adc_start(void *user, uint32_t rate_hz, rc_adc_cb_t cb, void *cb_user);
void rc_hal_virtual_adc_stop(void *user);
int rc_hal_virtual_digital_read(void *user, uint8_t channel, bool *level);
int rc_hal_virtual_bus_send(void *user, uint8_t port, const rc_bus_frame_t *frame);
int rc_hal_virtual_bus_receive_enable(void *user, uint8_t port, rc_bus_cb_t cb, void *cb_user);
int rc_hal_virtual_storage_read(void *user, uint32_t offset, void *dst, uint32_t len);
int rc_hal_virtual_storage_write(void *user, uint32_t offset, const void *src, uint32_t len);
uint32_t rc_hal_virtual_cycle_count(void *user);
void rc_hal_virtual_reset_reason(void *user, uint32_t *reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_HAL_VIRTUAL_H */
