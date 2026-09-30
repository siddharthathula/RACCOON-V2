/* RACCOON V2 - hardware abstraction boundary.
 *
 * The portable engine control core only ever talks to this interface.  Three
 * implementations exist:
 *   - firmware/hardware/hal_stm32h7.c   real silicon (STM32H733, LQFP144)
 *   - firmware/hardware/hal_virtual.c   logical time, synthesised inputs, captured
 *                                       outputs: unit tests, SIL and the crank
 *                                       simulator
 *   - hil/hal_hil.c                     the bench, which drives the real HAL
 *
 * Because the same core code compiles against all three, the tests exercise the shipping
 * firmware rather than a re-implementation of it.  That is the single most important
 * structural decision for testability (docs/TESTING_ARCHITECTURE.md section 1).
 */
#ifndef RC_HAL_H
#define RC_HAL_H

#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A captured position-sensor edge. */
typedef struct {
    uint32_t tick_us;    /**< microsecond capture timestamp */
    uint8_t channel;     /**< 0 = CKP, 1 = CMP */
    uint8_t level;       /**< sampled level at capture */
} rc_capture_edge_t;

/** One DMA-completed analog frame, phase-aligned to the crank. */
typedef struct {
    uint32_t tick_us;
    uint32_t sequence;
    uint16_t raw[RC_MAX_SENSORS];
    uint8_t count;
} rc_adc_frame_t;

/** A CAN or transport frame. */
typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    uint8_t extended;
    uint8_t fd;
} rc_bus_frame_t;

/** Input capture callback. Runs in interrupt context: bounded work only. */
typedef void (*rc_capture_cb_t)(const rc_capture_edge_t *edge, void *user);
/** ADC frame callback. Interrupt context: bounded work only. */
typedef void (*rc_adc_cb_t)(const rc_adc_frame_t *frame, void *user);
/** Bus receive callback. */
typedef void (*rc_bus_cb_t)(const rc_bus_frame_t *frame, void *user);

/**
 * The hardware interface.
 *
 * Every function takes a `void *ctx` as its first argument: the implementation owns its
 * state, and passing it explicitly (rather than hiding it in a global) is what lets the
 * unit tests and the SIL drive two or three virtual ECUs in one process.
 *
 * Every function returns 0 on success or a negative error code.  Functions the real-time
 * region may call must be non-blocking: `rc_hal_arm_event` writes a timer compare register
 * and returns, because the timer - not the CPU - produces the edge
 * (docs/FIRMWARE_ARCHITECTURE.md section 4).
 */
typedef struct {
    /* -- timing (L1/L2 safe: register access only) ---------------------------- */
    uint32_t (*tick_now_us)(void *ctx);
    /** Schedule an actuation edge. Returns 0 armed, <0 refused. */
    int (*arm_event)(void *ctx, const rc_event_t *event);
    /** Cancel any pending event on a channel, ignoring stale generations. */
    void (*disarm_event)(void *ctx, uint8_t type, uint8_t cylinder, uint16_t generation);
    /** Force a hardware-safe output state regardless of software. */
    void (*force_safe_output)(void *ctx, uint8_t type, uint8_t cylinder);

    /* Called by the driver when a compare actually produced its edge, from interrupt
     * context.  A scheduler that never hears this has to assume the edge it armed is
     * still pending, and will re-arm it - which on real hardware means the same edge
     * happens twice.  May be NULL when the caller does not need delivery notification. */
    void (*on_event_delivered)(void *ctx, const rc_event_t *event);

    /** The scheduler that owns this driver. The delivery interrupt reports to it, not to
     * the driver, so a driver cannot be shared between two owners by accident. */
    void *owner;

    /* -- capture (interrupt safe) --------------------------------------------- */
    int (*capture_enable)(void *ctx, uint8_t channel, rc_capture_cb_t cb, void *user);
    void (*capture_disable)(void *ctx, uint8_t channel);

    /* -- analog (L3 safe) ----------------------------------------------------- */
    int (*adc_start)(void *ctx, uint32_t rate_hz, rc_adc_cb_t cb, void *user);
    void (*adc_stop)(void *ctx);

    /* -- digital inputs (L3 safe) --------------------------------------------- */
    int (*digital_read)(void *ctx, uint8_t channel, bool *level);

    /* -- bus (L3/L4 safe) ----------------------------------------------------- */
    int (*bus_send)(void *ctx, uint8_t port, const rc_bus_frame_t *frame);
    int (*bus_receive_enable)(void *ctx, uint8_t port, rc_bus_cb_t cb, void *user);

    /* -- storage (L4 only; never called from L1/L2) --------------------------- */
    int (*storage_read)(void *ctx, uint32_t offset, void *dst, uint32_t len);
    int (*storage_write)(void *ctx, uint32_t offset, const void *src, uint32_t len);

    /* -- diagnostics ---------------------------------------------------------- */
    uint32_t (*cycle_count)(void *ctx);
    void (*reset_reason)(void *ctx, uint32_t *reason);
} rc_hal_t;

/** Error codes returned by the HAL. Negative, so a truthy test is a bug. */
#define RC_HAL_OK 0
#define RC_HAL_ERR_PARAM (-1)
#define RC_HAL_ERR_BUSY (-2)
#define RC_HAL_ERR_REFUSED (-3)
#define RC_HAL_ERR_TIMEOUT (-4)

/** Reset causes, latched across a reset (docs spec section 105). */
#define RC_RESET_POWER_ON 0u
#define RC_RESET_PIN 1u
#define RC_RESET_SOFTWARE 2u
#define RC_RESET_WATCHDOG 3u
#define RC_RESET_BROWNOUT 4u
#define RC_RESET_HARD_FAULT 5u
#define RC_RESET_UNKNOWN 6u

const char *rc_reset_reason_name(uint32_t reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_HAL_H */
