/* RACCOON V2 - fuel pipeline.
 *
 * One path, in one order, with every term separately observable:
 *
 *   load -> target lambda -> air mass -> fuel mass -> injector model -> transient
 *        -> closed loop -> cylinder trim -> performance -> torque -> SAFETY -> ms/stroke
 *
 * The hard rules this file exists to enforce (docs/FUEL_SPEC.md):
 *   F1  A fuel cut is FINAL.  Once the cut arbiter says cut, nothing downstream can
 *       re-open the injector for that event, including minimum-pulse and enrichment.
 *   F2  Small-pulse compensation only applies above the minimum pulse, so it can never
 *       substitute for a cut.
 *   F3  The pulse width is bounded by min/max pulse and by the duty ceiling, in software
 *       and in the hardware driver.
 *   F4  Closed-loop authority is bounded and its stability margin is measured, not
 *       assumed.
 *   F5  Every enrichment term has a maximum, a decay, and a log channel.
 *
 * DESIGN-NOTE
 *   Requirements: PRODUCT_REQUIREMENTS R-FUEL; docs/FUEL_SPEC.md.
 *   Public sources consulted: rusEFI's public documentation for the wall-wetting and
 *   tau concepts.  The pipeline structure, the cut arbiter and the bounds are original.
 *   INDEPENDENTLY AUTHORED (TPL-002).
 */
#ifndef RC_FUEL_H
#define RC_FUEL_H

#include "cal_types.h"
#include "rc/engine.h"
#include "rc/rc_math.h"
#include "rc/rc_types.h"
#include "rc/sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Inputs the fuel pipeline needs from the rest of the control loop. */
typedef struct {
    float rpm;
    float load;                 /**< primary load sensor value */
    uint8_t load_source;        /**< rc_load_source_t */
    float map_kpa;
    float baro_kpa;
    float iat_c;
    float ect_c;
    float battery_v;
    float fuel_pressure_kpa;
    float lambda;               /**< measured, 0 when no lambda is fitted */
    uint8_t lambda_valid;
    float tps_pct;
    float dt_s;                 /**< seconds since the previous cycle */
    uint8_t mode;               /**< active race mode index */
    bool crank_only;            /**< starter/cranking: no closed loop, no learner */
} rc_fuel_input_t;

/** Everything the pipeline decided, published so the log can explain the arithmetic. */
typedef struct {
    float target_lambda;
    float air_mass_g;           /**< per cycle */
    float fuel_mass_g;          /**< per cycle, before the injector model */
    float base_pulse_ms;        /**< after the injector model, before transient */
    float transient_pulse_ms;   /**< added by the transient model */
    float closed_loop_factor;   /**< multiplicative */
    float cylinder_trim;        /**< multiplicative */
    float duty_percent;
    bool fuel_cut;
    uint8_t cut_reason;
    bool closed_loop_active;
    bool learner_frozen;
    float trim[RC_MAX_CYLINDERS];
} rc_fuel_output_t;

/** Cut reasons, in the order they are evaluated. */
typedef enum {
    RC_CUT_NONE = 0,
    RC_CUT_DECELERATION = 1,
    RC_CUT_REV_LIMIT = 2,
    RC_CUT_LAUNCH = 3,
    RC_CUT_SHIFT = 4,
    RC_CUT_TRACTION = 5,
    RC_CUT_SAFETY = 6,
    RC_CUT_SENSOR_FAULT = 7,
    RC_CUT_KILL_SWITCH = 8,
    RC_CUT_PRESSURE = 9
} rc_cut_reason_t;

/** Long-lived state: the enrichment accumulator, the wall-wet film and the learner. */
typedef struct {
    float enrichment_pct;      /**< the single enrichment accumulator (F5) */
    float wall_wet_pct;         /**< film state, decays with tau */
    float previous_tps;
    float previous_map;
    bool decel_latched;
    uint32_t decel_since_us;
    float learner_trim[RC_MAX_CYLINDERS];
    uint8_t learner_cell[RC_MAX_CYLINDERS];
    bool learner_saturated;
    float learner_variance;
    uint16_t learner_samples;
} rc_fuel_state_t;

/** Initialise the persistent state. */
void rc_fuel_init(rc_fuel_state_t *state);

/**
 * Run the fuel pipeline.
 *
 * @param cut_active  a cut requested by an external owner (safety, torque manager,
 *                    performance feature).  This is a *hard gate*: it is applied after
 *                    every calculation, so no enrichment path can override it (F1).
 */
void rc_fuel_update(rc_fuel_state_t *state, const rc_cal_t *cal, const rc_engine_t *engine,
                    const rc_fuel_input_t *in, rc_cut_reason_t cut_active,
                    float torque_allowance, float safety_factor, rc_fuel_output_t *out);

/** Fuel model without the table, exposed for the SIL and for unit tests. */
float rc_fuel_pulse_ms(const rc_cal_t *cal, float fuel_mass_g, float battery_v,
                       float fuel_pressure_kpa);

/** Target lambda from the table, the ethanol blend and the cold enrichment. */
float rc_fuel_target_lambda(const rc_cal_t *cal, const rc_engine_t *engine,
                            const rc_fuel_input_t *in);

/** Volumetric efficiency lookup with a caller-supplied table (for tests and the tuner).
 *
 *  Takes the two axes as well as the cells, because the cells are meaningless without
 *  them: the breakpoints are what turn an rpm into a cell index. Passing engineering
 *  values where cell indices used to go is the mistake this signature exists to prevent -
 *  it used to take no axes, and 8,000 rpm silently became cell 15. */
float rc_fuel_ve_lookup(const void *cells, rc_cell_kind_t kind,
                        const void *rpm_axis, const void *map_axis,
                        float rpm, float map_kpa, float scale);

/** Stringify a cut reason for the log and the diagnostics view. */
const char *rc_fuel_cut_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_FUEL_H */
