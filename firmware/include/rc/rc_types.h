/* RACCOON V2 - core types.
 *
 * This header is included by every module, portable and target alike.  It must not
 * depend on any hardware header: the engine control core is compiled for the host so
 * that the tests and the SIL exercise the *shipping* code (docs/TESTING_ARCHITECTURE.md).
 *
 * DESIGN-NOTE
 *   Requirements: docs/ENGINE_CONTROL_SPEC.md, docs/TRIGGER_SPEC.md,
 *   docs/SCHEDULER (section 13), docs/SENSOR_SPEC.md, docs/TORQUE_CONTROL_SPEC.md.
 *   Public sources consulted: none for this file; the types are original.
 *   INDEPENDENTLY AUTHORED.
 */
#ifndef RC_TYPES_H
#define RC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ limits */

/** Supported cylinder range.  The product is 1..4; the core refuses anything else. */
#define RC_MAX_CYLINDERS 4u
/** Injector and ignition channels the control core can drive, independent of hardware. */
#define RC_MAX_INJECTORS 8u
#define RC_MAX_IGNITION 8u
/** Per-cylinder lambda channels. */
#define RC_MAX_LAMBDA 4u
/** Scheduler queue depth.  Sized for one full control cycle of every event type. */
#define RC_SCHEDULER_QUEUE 64u
/** Sensor channels the control core tracks. */
#define RC_MAX_SENSORS 32u

/* -------------------------------------------------------------- trigger states */

/** The ten decoder states of docs/TRIGGER_SPEC.md section 4. Order is normative. */
typedef enum {
    RC_TRIGGER_NO_SIGNAL = 0,
    RC_TRIGGER_INITIALIZING = 1,
    RC_TRIGGER_SEEKING_SYNC = 2,
    RC_TRIGGER_SYNC_CANDIDATE = 3,
    RC_TRIGGER_SYNCED = 4,
    RC_TRIGGER_DEGRADED = 5,
    RC_TRIGGER_SYNC_LOST = 6,
    RC_TRIGGER_RECOVERING = 7,
    RC_TRIGGER_FAULT = 8,
    RC_TRIGGER_STATE_COUNT = 9
} rc_trigger_state_t;

/** True when the decoder state permits fuel and spark to be commanded. */
static inline bool rc_trigger_allows_control(rc_trigger_state_t state)
{
    return state == RC_TRIGGER_SYNCED || state == RC_TRIGGER_DEGRADED;
}

/** True when the engine is turning fast enough that RPM is meaningful. */
static inline bool rc_trigger_has_rpm(rc_trigger_state_t state)
{
    return state == RC_TRIGGER_SYNCED || state == RC_TRIGGER_DEGRADED
        || state == RC_TRIGGER_SYNC_CANDIDATE;
}

/* ----------------------------------------------------------------- position */

/** High-resolution engine position model (docs/TRIGGER_SPEC.md section 5). */
typedef struct {
    uint32_t rpm_x2;             /**< filtered speed, 2x RPM for resolution */
    uint16_t crank_angle_deg10;  /**< 0..3599, 0.1 degree resolution */
    uint16_t cycle_angle_deg10;  /**< 0..7199 for a four-stroke */
    uint16_t tooth_index;
    uint32_t tooth_period_us;    /**< as measured on this edge */
    uint32_t period_filtered_us; /**< used for prediction */
    uint8_t sync_state;          /**< rc_trigger_state_t */
    uint8_t sync_confidence;     /**< 0..100 */
    uint8_t cylinder_phase;      /**< index into the firing order, not the cylinder */
    int32_t accel_rpm_per_s;     /**< filtered d(rpm)/dt, bounded */
    int32_t decel_rpm_per_s;     /**< filtered -d(rpm)/dt, bounded */
} rc_engine_position_t;

/* -------------------------------------------------------------------- events */

/** Scheduler event types (docs spec section 13). */
typedef enum {
    RC_EV_INJECTOR_OPEN = 0,
    RC_EV_INJECTOR_CLOSE = 1,
    RC_EV_COIL_CHARGE = 2,
    RC_EV_COIL_FIRE = 3,
    RC_EV_VVT = 4,
    RC_EV_IDLE_ACTUATOR = 5,
    RC_EV_BOOST = 6,
    RC_EV_FUEL_CUT = 7,
    RC_EV_IGNITION_CUT = 8,
    RC_EV_LAUNCH = 9,
    RC_EV_SHIFT_CUT = 10,
    RC_EV_TRACTION = 11,
    RC_EV_AUX = 12,
    RC_EV_TYPE_COUNT = 13
} rc_event_type_t;

typedef enum {
    RC_EV_PENDING = 0,
    RC_EV_ARMED = 1,
    RC_EV_FIRED = 2,
    RC_EV_CANCELLED = 3,
    RC_EV_EXPIRED = 4
} rc_event_state_t;

/** The canonical event record. Invariants are listed in FIRMWARE_ARCHITECTURE.md 4.2. */
typedef struct {
    uint8_t type;         /**< rc_event_type_t */
    uint8_t cylinder;    /**< 0..RC_MAX_CYLINDERS-1, or 0 for engine-wide */
    uint8_t state;       /**< rc_event_state_t */
    uint8_t priority;    /**< 0 = highest */
    uint16_t generation; /**< bumped on every re-plan; stale events are dropped */
    uint32_t target_tick; /**< absolute microsecond tick */
    uint32_t deadline;   /**< latest acceptable execution tick, wrap-safe */
} rc_event_t;

/* ------------------------------------------------------------------- sensors */

/** Sensor fault bitmask (docs/SENSOR_SPEC.md section 2). */
#define RC_SENSOR_F_OPEN (1u << 0)
#define RC_SENSOR_F_SHORT (1u << 1)
#define RC_SENSOR_F_FROZEN (1u << 2)
#define RC_SENSOR_F_STALE (1u << 3)
#define RC_SENSOR_F_RATE (1u << 4)
#define RC_SENSOR_F_RANGE (1u << 5)
#define RC_SENSOR_F_INCONSISTENT (1u << 6)
#define RC_SENSOR_F_ADC (1u << 7)
#define RC_SENSOR_F_DMA (1u << 8)
#define RC_SENSOR_F_REFERENCE (1u << 9)
#define RC_SENSOR_F_TIMEOUT (1u << 10)

/** What the engine control does when a sensor is not usable. */
typedef enum {
    RC_SENSOR_FB_HOLD = 0,      /**< substitute the last good value, flag it */
    RC_SENSOR_FB_SUBSTITUTE = 1,/**< substitute a declared safe value */
    RC_SENSOR_FB_LIMIT = 2,     /**< clamp the downstream request */
    RC_SENSOR_FB_CUT = 3,       /**< stop injecting */
    RC_SENSOR_FB_SHUTDOWN = 4   /**< stop the engine */
} rc_sensor_fault_behaviour_t;

/** The single sensor value contract every sensor type produces. */
typedef struct {
    float value;            /**< engineering units */
    uint32_t timestamp_us;  /**< engine time */
    uint32_t sequence;      /**< monotonic per sensor; freshness is based on this */
    uint8_t valid;          /**< usable as-is */
    uint8_t fresh;          /**< updated within the freshness window */
    uint8_t range_valid;
    uint8_t plausible;
    uint16_t fault;         /**< RC_SENSOR_F_* bitmask; 11 bits are defined */
    float fallback;         /**< value substituted when not valid */
    uint8_t using_fallback;
} rc_sensor_value_t;

/* ------------------------------------------------------------ safety actions */

/** Protective actions, ordered by severity (docs spec section 49). */
typedef enum {
    RC_ACT_NONE = 0,
    RC_ACT_WARNING = 1,
    RC_ACT_RETARD = 2,
    RC_ACT_ENRICH = 3,
    RC_ACT_LIMIT = 4,
    RC_ACT_CUT = 5,
    RC_ACT_SHUTDOWN = 6
} rc_action_t;

/** Active hard limits, as a bitmask so a log can explain every action at once. */
#define RC_SAFETY_RPM (1u << 0)
#define RC_SAFETY_OVERSPEED (1u << 1)
#define RC_SAFETY_LEAN (1u << 2)
#define RC_SAFETY_RICH (1u << 3)
#define RC_SAFETY_ECT (1u << 4)
#define RC_SAFETY_IAT (1u << 5)
#define RC_SAFETY_OIL_PRESSURE (1u << 6)
#define RC_SAFETY_OIL_TEMP (1u << 7)
#define RC_SAFETY_EGT (1u << 8)
#define RC_SAFETY_BATTERY (1u << 9)
#define RC_SAFETY_INJECTOR_DUTY (1u << 10)
#define RC_SAFETY_DWELL (1u << 11)
#define RC_SAFETY_FUEL_PRESSURE (1u << 12)
#define RC_SAFETY_SYNC (1u << 13)
#define RC_SAFETY_BOOST (1u << 14)
#define RC_SAFETY_SENSOR (1u << 15)

/* --------------------------------------------------------------- correction */

/** Every multiplicative or additive term in the pipeline, kept separately so the
 * engine's behaviour is explainable from a log (docs/ENGINE_CONTROL_SPEC.md 6). */
typedef struct {
    float base_map;        /**< VE or ignition base table value */
    float sensor_corr;     /**< IAT/ECT/BARO/BAT/ETHANOL */
    float strategy_factor; /**< load strategy and fuel model */
    float injector_model;  /**< dead time, battery, pressure, min/max pulse */
    float transient;       /**< acceleration enrichment / wall wetting */
    float closed_loop;     /**< lambda correction */
    float cylinder_trim;   /**< per-cylinder learned or measured trim */
    float performance;     /**< launch/TC/shift requester contribution */
    float torque_allowance;/**< arbitration result, 0..1 */
    float safety_factor;   /**< hard envelope result, 0..1 */
} rc_correction_vector_t;

/* ------------------------------------------------------------- engine state */

/** The published engine state. Doubly buffered; see FIRMWARE_ARCHITECTURE.md 2. */
typedef struct {
    rc_engine_position_t pos;
    uint32_t sequence;
    uint32_t engine_time_us;
    uint32_t cycle_count;

    float load;                    /**< primary load sensor value */
    uint8_t load_source;           /**< which strategy produced load */
    float load_confidence;
    float baro_kpa;
    float lambda[RC_MAX_LAMBDA];   /**< per channel, engineering lambda */
    uint8_t lambda_valid[RC_MAX_LAMBDA];

    float injector_ms[RC_MAX_INJECTORS];
    float ignition_deg[RC_MAX_IGNITION];
    float dwell_ms[RC_MAX_IGNITION];
    uint8_t ignition_cut[RC_MAX_IGNITION];
    /** Commanded idle air, percent. Published because it is a real output: a tuner has to
     *  be able to see the valve the ECU is driving, and a plant or a test has to be able to
     *  apply it. */
    float idle_valve_pct;

    /** Raw sensor readings, as the control loop actually read them this cycle.
     *
     *  Published rather than left inside the sensor module because the live-data frame
     *  declares these fields. A frame that promises a coolant temperature and then sends a
     *  constant zero is worse than a frame with no such field: nothing about it looks
     *  broken, so a tuner logs a flat zero coolant reading and a diagnostic concludes the
     *  sensor is disconnected when in fact the ECU never read it. */
    struct {
        float tps_pct;
        float iat_c;
        float ect_c;
        float battery_v;
        float fuel_pressure_kpa;
        float oil_pressure_bar;
        float oil_temp_c;
    } raw;

    rc_correction_vector_t corr;
    uint32_t arb_trace;     /**< which torque requester won */
    uint32_t safety_active; /**< RC_SAFETY_* bitmask */
    rc_action_t action;     /**< most severe active action */
    uint8_t armed;          /**< engine control armed */
} rc_engine_state_t;

/** Load source, echoed in telemetry so a log can explain the fuel model. */
typedef enum {
    RC_LOAD_MAP = 0,
    RC_LOAD_TPS = 1,
    RC_LOAD_MAF = 2,
    RC_LOAD_HYBRID = 3,
    RC_LOAD_INVALID = 4
} rc_load_source_t;

/* ------------------------------------------------------------------- helpers */

/** Wrap-safe forward difference for a free-running 32-bit microsecond counter. */
static inline uint32_t rc_ticks_forward(uint32_t now, uint32_t then)
{
    return (uint32_t)(now - then);
}

/** True when `deadline` has not yet passed at `now`, using wrap-safe comparison. */
static inline bool rc_deadline_ok(uint32_t now, uint32_t deadline)
{
    return (int32_t)(deadline - now) >= 0;
}

/** Degrees to microseconds at a given rate, saturating rather than overflowing. */
static inline uint32_t rc_degrees_to_us(float degrees, uint32_t rpm)
{
    if (rpm == 0u || degrees <= 0.0f) {
        return 0u;
    }
    /* us = degrees / 360 * 60e6 / rpm = degrees * 166666.67 / rpm */
    const double us = ((double)degrees * 166666.66666666666) / (double)rpm;
    if (us <= 0.0) {
        return 0u;
    }
    if (us >= 4.0e9) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)us;
}

#ifdef __cplusplus
}
#endif
#endif /* RC_TYPES_H */
