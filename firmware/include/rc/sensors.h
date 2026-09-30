/* RACCOON V2 - sensor framework.
 *
 * Every sensor, whatever its physical type, produces the same rc_sensor_value_t.  That is
 * what makes validation, fallback, logging and the tuner UI uniform, and it is what makes
 * the "a stale value must never silently stay valid" rule (spec section 16) enforceable
 * in one place instead of in sixteen.
 *
 * The critical distinction, which production ECUs routinely get wrong:
 *   validity  is STRUCTURAL  - open, short, stale, frozen, out of range, ADC/DMA failure
 *   plausibility is DIAGNOSTIC - a rationality check against a paired sensor
 * Conflating them produces false shutdowns, so they are separate flags and only validity
 * may gate the control path.
 */
#ifndef RC_SENSORS_H
#define RC_SENSORS_H

#include "cal_types.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-channel configuration, normally derived from calibration once at load. */
/** The converter these transfer functions are written against.  A slope is always
 *  expressed in engineering units per COUNT of this converter, whatever unit the sensor is
 *  described in; see the MAP entry in control.c for the conversion. */
#define RC_ADC_FULL_SCALE 4095.0f
#define RC_ADC_REFERENCE_V 5.0f

typedef struct {
    float slope;            /**< engineering units per raw ADC count */
    float offset;           /**< added after scaling */
    float min;              /**< physical lower bound */
    float max;              /**< physical upper bound */
    float max_rate;         /**< engineering units per second */
    uint32_t freshness_us;  /**< a value not refreshed within this is stale */
    uint8_t behaviour;      /**< rc_sensor_fault_behaviour_t */
    uint8_t enabled;
    /** How long the reading must be bit-identical before it is called frozen.
     *
     *  This has to be a real number per vehicle rather than a constant, because it trades
     *  two real failures against each other: too short and a sensor sitting at equilibrium
     *  is declared broken, which invalidates the input the engine is running on; too long
     *  and a genuinely seized channel is trusted for longer than it should be. */
    uint32_t frozen_window_us;
} rc_sensor_config_t;

typedef struct {
    rc_sensor_value_t value;
    rc_sensor_config_t config;

    /* internal */
    float previous;
    uint32_t last_sequence;
    uint32_t last_change_us;
    float rate;
    bool rate_limited;
    uint8_t open_circuit_count;
    uint8_t short_circuit_count;
} rc_sensor_t;

typedef struct {
    rc_sensor_t channel[RC_MAX_SENSORS];
    uint8_t count;
    /** Sensor-health bitmask for the ECU health page and the freeze frame. */
    uint32_t health_mask;
    uint32_t fault_count;
} rc_sensors_t;

/** Initialise from a zeroed configuration; safe defaults are fail-safe. */
void rc_sensors_init(rc_sensors_t *sensors);

/** Register a channel. Returns its index, or 0xFF when full. */
uint8_t rc_sensors_add(rc_sensors_t *sensors, const rc_sensor_config_t *config);

/** Number of registered channels. */
uint8_t rc_sensors_count(const rc_sensors_t *sensors);

/**
 * Update one channel from a raw ADC reading.
 *
 * Applies the transfer function, then every validity check in this order:
 *   1. structural open/short against the calibrated open/short codes
 *   2. range
 *   3. frozen (the sequence has not advanced)
 *   4. rate of change
 *
 * @param raw    ADC counts
 * @param now_us engine time
 * @param seq    frame sequence number, which must advance for the value to be fresh
 */
void rc_sensors_update(rc_sensors_t *sensors, uint8_t index, uint16_t raw,
                       uint32_t now_us, uint32_t seq);

/** Force a channel invalid with a reason, e.g. from a DMA or ADC fault. */
void rc_sensors_invalidate(rc_sensors_t *sensors, uint8_t index, uint16_t fault_bit);

/**
 * Invalidate every channel that depends on the 5 V reference.
 *
 * A collapsed sensor rail silently rescales every 5 V sensor at once; unless the
 * reference itself is monitored, the control loop would compute a plausible-looking
 * number from a broken front end. This is the reason `ref5v` exists (SENSOR_SPEC 4).
 */
void rc_sensors_reference_fault(rc_sensors_t *sensors, uint8_t ref_index, bool railed);

/** Cross-check two channels that must agree; sets the INCONSISTENT bit on both. */
void rc_sensors_cross_check(rc_sensors_t *sensors, uint8_t a, uint8_t b, float tolerance);

/** Value to use downstream: the real value, or the declared fallback. */
float rc_sensors_value(const rc_sensor_t *sensor);

/** True when the channel is usable as-is. */
bool rc_sensors_usable(const rc_sensor_t *sensor);

/** Human-readable fault summary, for the diagnostics workspace. */
const char *rc_sensors_fault_text(uint16_t fault);

#ifdef __cplusplus
}
#endif
#endif /* RC_SENSORS_H */
