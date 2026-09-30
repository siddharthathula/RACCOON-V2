/* RACCOON V2 - sensor framework. See sensors.h. */
#include "rc/sensors.h"

#include <math.h>
#include <string.h>

#include "rc/rc_math.h"

#define RC_ADC_MAX 4095.0f
/* A reading this close to either rail is treated as a wiring fault rather than data. */
#define RC_OPEN_LOW_FRACTION 0.005f
#define RC_OPEN_HIGH_FRACTION 0.995f
/* A value that has not moved at all for this long, and has a valid sequence, is frozen. */
/* Floor for the frozen window, used only when the calibration has not set one.  Five
 * milliseconds is not a defensible value: a throttle at rest, or a manifold pressure at a
 * steady idle, produces an identical reading many times in a row, and a detector that
 * calls that a stuck sensor invalidates the input that the engine is running on.  A real
 * analog-to-digital converter always has at least a count or two of noise, so a genuinely
 * frozen channel is one that never moves at all, for a long time. */
/* Used when nothing has configured a window.  A real vehicle wants seconds, not
 * milliseconds: equilibrium looks exactly like a fault if the window is short.  An
 * explicitly configured window is always honoured - the default exists to cover the unset
 * case, not to overrule a deliberate choice. */
#define RC_FROZEN_US_DEFAULT 2000000u

void rc_sensors_init(rc_sensors_t *sensors)
{
    if (sensors == NULL) {
        return;
    }
    memset(sensors, 0, sizeof(*sensors));
}

uint8_t rc_sensors_add(rc_sensors_t *sensors, const rc_sensor_config_t *config)
{
    if (sensors == NULL || config == NULL) {
        return 0xFFu;
    }
    if (sensors->count >= RC_MAX_SENSORS) {
        return 0xFFu;
    }
    const uint8_t index = sensors->count++;
    rc_sensor_t *sensor = &sensors->channel[index];
    memset(sensor, 0, sizeof(*sensor));
    sensor->config = *config;
    sensor->value.fallback = config->min;
    sensor->value.valid = 0u;
    sensor->value.fresh = 0u;
    sensor->value.plausible = 1u;
    return index;
}

uint8_t rc_sensors_count(const rc_sensors_t *sensors)
{
    return (sensors != NULL) ? sensors->count : 0u;
}

static void set_fault(rc_sensors_t *sensors, rc_sensor_t *sensor, uint16_t bit, uint8_t index)
{
    if ((sensor->value.fault & bit) == 0u) {
        sensors->fault_count++;
    }
    sensor->value.fault |= bit;
    sensor->value.valid = 0u;
    sensor->value.fresh = 0u;
    sensor->value.using_fallback = 1u;
    sensor->value.fallback = sensor->config.min;
    (void)index;
}

void rc_sensors_update(rc_sensors_t *sensors, uint8_t index, uint16_t raw,
                       uint32_t now_us, uint32_t seq)
{
    if (sensors == NULL || index >= sensors->count) {
        return;
    }
    rc_sensor_t *sensor = &sensors->channel[index];
    const rc_sensor_config_t *cfg = &sensor->config;

    if (cfg->enabled == 0u) {
        sensor->value.valid = 0u;
        sensor->value.fresh = 0u;
        sensor->value.fault = RC_SENSOR_F_OPEN; /* an absent sensor is an open circuit */
        return;
    }

    sensor->value.timestamp_us = now_us;
    sensor->value.sequence = seq;

    /* 1. structural: open / short against the rails. */
    const float fraction = (float)raw / RC_ADC_MAX;
    const bool near_low = fraction <= RC_OPEN_LOW_FRACTION;
    const bool near_high = fraction >= RC_OPEN_HIGH_FRACTION;
    if (near_low || near_high) {
        set_fault(sensors, sensor,
                  (uint8_t)((near_low) ? RC_SENSOR_F_OPEN : RC_SENSOR_F_SHORT), index);
        if (near_low) {
            sensor->open_circuit_count++;
        } else {
            sensor->short_circuit_count++;
        }
        return;
    }

    /* 2. transfer function, then range. */
    const float engineering = ((float)raw * cfg->slope) + cfg->offset;
    sensor->value.value = engineering;
    if (!(engineering >= cfg->min) || !(engineering <= cfg->max)) {
        set_fault(sensors, sensor, RC_SENSOR_F_RANGE, index);
        return;
    }
    sensor->value.range_valid = 1u;

    /* 3. freshness and frozen.  Freshness is decided by the sequence, not by the value
     *    looking reasonable: a frozen sensor produces a perfectly plausible number. */
    /* Frozen means the VALUE stopped moving, which is a different failure from a stalled
     * sequence below.  A stuck ADC register or a wedged filter keeps producing the same
     * perfectly plausible number while the acquisition pipeline happily counts frames, so
     * the sequence advances and the reading is still a lie.  Keying this off the sequence
     * would make it undetectable exactly when it matters most. */
    /* The sequence check comes FIRST.  A repeated sequence is a stalled acquisition
     * pipeline, which is a specific and actionable diagnosis; reporting it as "frozen"
     * would send someone looking at a sensor that is behaving perfectly and a wiring
     * problem that is not there. */
    if (sensor->last_sequence != 0u && seq == sensor->last_sequence) {
        set_fault(sensors, sensor, RC_SENSOR_F_STALE, index);
        return;
    }
    {
        const uint32_t window = (cfg->frozen_window_us > 0u) ? cfg->frozen_window_us
                                                            : RC_FROZEN_US_DEFAULT;
        if (sensor->last_change_us != 0u
            && (now_us - sensor->last_change_us) > window
            && rc_absf(engineering - sensor->previous) < 1.0e-6f) {
            set_fault(sensors, sensor, RC_SENSOR_F_FROZEN, index);
            return;
        }
    }
    if (sensor->last_sequence == 0u || seq != sensor->last_sequence) {
        sensor->value.fresh = 1u;
    }
    sensor->last_sequence = seq;

    /* 4. rate of change.  A limiter with a visible flag, so a real fast transient stays
     *    distinguishable from a physically impossible one. */
    bool limited = false;
    const float previous = sensor->previous;
    const float raw_value = engineering;
    if (sensor->last_change_us != 0u) {
        const float dt = (float)(now_us - sensor->last_change_us) * 1.0e-6f;
        if (dt > 0.0f) {
            sensor->rate = (raw_value - previous) / dt;
            /* Compared with a relative tolerance.  A signal sitting exactly at its
             * configured maximum rate is not exceeding it, and in single precision the
             * computed rate lands a fraction either side of the limit - so a strict `>`
             * makes the limiter flag and unflag the same steady signal forever. */
            const float rate_magnitude = rc_absf(sensor->rate);
            const float tolerance = rc_absf(cfg->max_rate) * 1.0e-3f;
            if (cfg->max_rate > 0.0f && (rate_magnitude > (cfg->max_rate + tolerance))) {
                sensor->value.fault |= (uint16_t)RC_SENSOR_F_RATE;
                limited = true;
            }
        }
    }
    sensor->rate_limited = limited;
    /* RATE is cleared only here, by a rate that is actually within limits.  Clearing it in
     * the blanket "a good reading clears structural faults" mask below would erase the flag
     * in the same call that set it, so an impossible rate would never be reportable. */
    if (!limited) {
        sensor->value.fault &= (uint16_t)~(uint16_t)RC_SENSOR_F_RATE;
    }
    if (limited) {
        /* The flag is set, but the value is still passed through: a genuine event
         * (a backfire, a snap over-run) is exactly what a rate limit is for, and hiding
         * it would make the log lie. */
        /* The signal DID move, so the baseline must advance.  Holding `previous` back
         * would measure every later rate against the pre-spike value, which re-flags the
         * channel indefinitely and turns one transient into a permanent rate fault. */
        sensor->previous = raw_value;
        sensor->last_change_us = now_us;
    } else {
        sensor->previous = raw_value;
        /* The change timestamp only moves when the value ACTUALLY moved.  Advancing it on
         * every sample resets the frozen timer forever, so a genuinely stuck sensor - the
         * exact case this detector exists for - is never detected at all. */
        if (rc_absf(raw_value - previous) >= 1.0e-6f) {
            sensor->last_change_us = now_us;
        }
    }

    /* Plausibility is diagnostic only, so it never gates the control path. */
    sensor->value.plausible = 1u;
    sensor->value.valid = 1u;
    sensor->value.using_fallback = 0u;
    /* Every *structural* fault is cleared by a good reading.  Leaving them sticky would
     * mean a channel that faulted once could never become valid again, which is how a
     * transient glitch becomes a permanent limp.  INCONSISTENT is left alone: it belongs
     * to the cross-check, not to this channel. */
    sensor->value.fault &= (uint16_t)~(uint16_t)(RC_SENSOR_F_OPEN | RC_SENSOR_F_SHORT
                                                | RC_SENSOR_F_FROZEN | RC_SENSOR_F_STALE
                                                | RC_SENSOR_F_RANGE | RC_SENSOR_F_ADC
                                                | RC_SENSOR_F_DMA | RC_SENSOR_F_REFERENCE
                                                | RC_SENSOR_F_TIMEOUT);
    sensors->health_mask &= ~(1u << index);
    sensors->health_mask |= 1u << index;
}

void rc_sensors_invalidate(rc_sensors_t *sensors, uint8_t index, uint16_t fault_bit)
{
    if (sensors == NULL || index >= sensors->count) {
        return;
    }
    set_fault(sensors, &sensors->channel[index], fault_bit, index);
}

void rc_sensors_reference_fault(rc_sensors_t *sensors, uint8_t ref_index, bool railed)
{
    if (sensors == NULL) {
        return;
    }
    if (ref_index < sensors->count) {
        rc_sensor_t *reference = &sensors->channel[ref_index];
        if (railed) {
            set_fault(sensors, reference, RC_SENSOR_F_REFERENCE, ref_index);
        } else {
            reference->value.fault &= (uint16_t)~(uint16_t)RC_SENSOR_F_REFERENCE;
        }
    }
    if (!railed) {
        return;
    }
    /* Every channel that depends on the reference is now meaningless.  Invalidate them
     * all rather than letting each one discover the problem independently. */
    for (uint8_t i = 0u; i < sensors->count; ++i) {
        if (i == ref_index) {
            continue;
        }
        if (sensors->channel[i].config.enabled == 0u) {
            continue;
        }
        if (railed) {
            set_fault(sensors, &sensors->channel[i], RC_SENSOR_F_REFERENCE, i);
        } else {
            /* Clearing the rail clears the dependents it invalidated.  Leaving the fault
             * latched here would mean a rail that blipped for one sample costs the engine
             * every 5 V sensor until an ECU reset, which is a far harsher response than
             * the fault itself deserves. */
            sensors->channel[i].value.fault &= (uint16_t)~(uint16_t)RC_SENSOR_F_REFERENCE;
        }
    }
}

void rc_sensors_cross_check(rc_sensors_t *sensors, uint8_t a, uint8_t b, float tolerance)
{
    if (sensors == NULL || a >= sensors->count || b >= sensors->count) {
        return;
    }
    rc_sensor_t *first = &sensors->channel[a];
    rc_sensor_t *second = &sensors->channel[b];
    if (first->value.valid == 0u || second->value.valid == 0u) {
        return;
    }
    if (rc_absf(first->value.value - second->value.value) > tolerance) {
        /* Diagnostic, not structural: a mismatch is reported but the values stay usable,
         * because a dual-channel sensor disagreement is information for the tuner, not
         * proof that either channel is broken. */
        first->value.plausible = 0u;
        second->value.plausible = 0u;
        first->value.fault |= (uint16_t)RC_SENSOR_F_INCONSISTENT;
        second->value.fault |= (uint16_t)RC_SENSOR_F_INCONSISTENT;
    } else {
        first->value.plausible = 1u;
        second->value.plausible = 1u;
        first->value.fault &= (uint16_t)~(uint16_t)RC_SENSOR_F_INCONSISTENT;
        second->value.fault &= (uint16_t)~(uint16_t)RC_SENSOR_F_INCONSISTENT;
    }
}

float rc_sensors_value(const rc_sensor_t *sensor)
{
    if (sensor == NULL) {
        return 0.0f;
    }
    return (sensor->value.valid != 0u) ? sensor->value.value : sensor->value.fallback;
}

bool rc_sensors_usable(const rc_sensor_t *sensor)
{
    return (sensor != NULL) && (sensor->value.valid != 0u) && (sensor->value.fresh != 0u)
        && (sensor->value.range_valid != 0u);
}

const char *rc_sensors_fault_text(uint16_t fault)
{
    if ((fault & RC_SENSOR_F_OPEN) != 0u) {
        return "open circuit";
    }
    if ((fault & RC_SENSOR_F_SHORT) != 0u) {
        return "short circuit";
    }
    if ((fault & RC_SENSOR_F_FROZEN) != 0u) {
        return "frozen value";
    }
    if ((fault & RC_SENSOR_F_STALE) != 0u) {
        return "stale, frame pipeline stalled";
    }
    if ((fault & RC_SENSOR_F_RATE) != 0u) {
        return "rate limited";
    }
    if ((fault & RC_SENSOR_F_RANGE) != 0u) {
        return "out of range";
    }
    if ((fault & RC_SENSOR_F_INCONSISTENT) != 0u) {
        return "channel mismatch";
    }
    if ((fault & RC_SENSOR_F_ADC) != 0u) {
        return "ADC fault";
    }
    if ((fault & RC_SENSOR_F_DMA) != 0u) {
        return "DMA fault";
    }
    if ((fault & RC_SENSOR_F_REFERENCE) != 0u) {
        return "sensor reference rail fault";
    }
    if ((fault & RC_SENSOR_F_TIMEOUT) != 0u) {
        return "timeout";
    }
    return "ok";
}
