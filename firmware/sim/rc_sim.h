/* RACCOON V2 - virtual engine, rider and road.
 *
 * This is a SOFTWARE-IN-THE-LOOP plant model.  Its purpose is to exercise the ECU: to
 * drive it through states a bench never reaches, and to find crashes, NaNs and
 * non-terminating loops in code that unit tests only ever hand convenient numbers.
 *
 * It is deliberately NOT a validated physics model.  The constants are order-of-magnitude
 * for a small air-cooled parallel twin, chosen so the engine is stable, starts, revs and
 * stalls at believable numbers.  Nothing here is evidence about a real engine, and no
 * tuning decision may be justified from it.  Where the model is wrong in an interesting
 * way it says so at the point of the error.
 *
 * The physical fidelity that actually matters for testing the ECU is the *coupling*:
 *   - an injector pulse shorter than the injector dead time delivers NO fuel, which is why
 *     the minimum-pulse rule exists and why the model has to reproduce it;
 *   - a spark that is cut or never commanded produces no torque;
 *   - mixture depends on what was actually injected, not on what was requested;
 *   - oil pressure falls with engine speed, so a stall trips the oil protection.
 */
#ifndef RC_SIM_H
#define RC_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/rc_hal.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_SIM_MAX_EVENTS 64u
#define RC_SIM_CYLINDERS 4u

/** What the rider and the world are doing.  Every field is a command, never a result. */
typedef struct {
    float throttle_pct;      /**< rider throttle demand, 0..100 */
    float brake_pct;         /**< front brake, 0..100 */
    bool clutch_engaged;     /**< true = clutch in */
    bool starter_engaged;    /**< true = starter motor turning the crank */
    uint8_t gear;            /**< 1..N, 0 = neutral */
    bool launch_button;
    bool shift_up_requested;
    bool shift_down_requested;
} rc_sim_rider_t;

/** Faults the harness can inject into the plant without touching the ECU. */
typedef struct {
    bool tps_stuck_low;      /**< reports a plausible but wrong TPS */
    bool tps_stuck_high;
    bool map_rail;           /**< the 5 V reference collapses */
    bool map_open;
    bool ect_open;
    bool oil_sensor_stuck;   /**< a frozen reading, plausible forever */
    bool lambda_noisy;
    /** A constant offset applied to what the wideband REPORTS, with no change to the
     *  mixture.  A wideband that disagrees with the engine is a real failure mode - a
     *  fouled sensor, a miscalibrated one, exhaust gas recirculation - and it is the only
     *  way to check that the closed loop reacts to the MEASUREMENT rather than to the
     *  truth, which is all it is ever given. */
    float lambda_bias;
    float battery_v;         /**< supply voltage, 0 = nominal 13.8 */
    float injector_flow_scale; /**< 1.0 = as calibrated; wrong flow is a classic tune error */
    float trigger_noise;     /**< probability of a spurious crank edge per step */
} rc_sim_faults_t;

/** The plant. */
typedef struct {
    /* --- geometry, fixed at init ------------------------------------------ */
    float swept_cc;          /**< per cylinder */
    uint8_t cylinders;
    float compression_ratio;
    /* --- crankshaft ------------------------------------------------------- */
    float crank_angle_deg;   /**< 0..720, wrapping */
    float rpm;
    float omega_rad_s;       /**< rad/s, integrated */
    bool running;
    bool fired_this_cycle;   /**< set when any cylinder combusted in the last cycle */

    /* --- charge and mixture ----------------------------------------------- */
    float map_kpa;
    float baro_kpa;
    float iat_c;
    float ect_c;
    float oil_temp_c;
    float oil_pressure_bar;
    float egt_c;
    float lambda;            /**< measured, exhaust side */
    float lambda_true;       /**< what the mixture actually is */
    float afr_actual;
    float air_mass_g_cycle;
    float fuel_mass_g_cycle;
    float fuel_delivered_g;  /**< total injected, for mass balance */
    float cycle_fuel_g;       /**< fuel accumulated over the current revolution */
    float last_crank_deg;     /**< to detect the revolution boundary */

    /* --- outputs the ECU commanded this cycle ----------------------------- */
    float injector_open_ms[RC_SIM_CYLINDERS];
    float dwell_ms[RC_SIM_CYLINDERS];
    float ignition_deg[RC_SIM_CYLINDERS];
    bool spark_cut[RC_SIM_CYLINDERS];
    bool fuel_cut;
    uint32_t spark_count;
    uint32_t injector_pulses;
    uint32_t dead_time_pulses; /**< pulses too short to open the injector */

    /* --- drivetrain -------------------------------------------------------- */
    float speed_kph;
    float idle_valve_pct;    /**< commanded by the ECU, applied as a throttle bypass */
    float wheel_speed_kph;
    float front_wheel_kph;
    float rear_wheel_kph;

    /* --- bookkeeping ------------------------------------------------------- */
    uint32_t now_us;
    uint32_t step_count;
    float peak_rpm;
    float lean_event_count;
    float stall_count;
    bool knock_flag;
    float knock_margin;      /**< > 0 means knock detected */
} rc_sim_t;

/* --------------------------------------------------------------- plant model */

/** Reset the plant to a cold, stopped engine. */
void rc_sim_init(rc_sim_t *sim, float swept_cc_per_cylinder, uint8_t cylinders,
                 float compression_ratio);

/** Advance the plant by `dt_s` seconds of simulated time.
 *
 *  The ECU is *not* called from here: the harness owns the loop so it can interleave the
 *  trigger edges, the ADC frame and the control cycle in the order real hardware would. */
void rc_sim_step(rc_sim_t *sim, const rc_sim_rider_t *rider, const rc_sim_faults_t *faults,
                 float dt_s);

/** Advance exactly to the crank angle where the next CKP tooth falls, and report how long
 *  that took.  This is how the harness generates a real trigger waveform. */
float rc_sim_time_to_next_ckp(const rc_sim_t *sim, uint8_t teeth, uint8_t missing);

/** The crank angle at which the next CKP edge is expected, in degrees 0..720. */
float rc_sim_next_ckp_angle(const rc_sim_t *sim, uint8_t teeth, uint8_t missing);

/** Time until the next CMP edge, and its angle.  cam_teeth == 0 means no cam fitted. */
float rc_sim_time_to_next_cmp(const rc_sim_t *sim, uint8_t cam_teeth, float *angle_out);

/** The ECU commanded fuel; the plant consumes it.  This is the only path by which fuel
 *  enters the simulated engine, which is what makes an over-rich tune actually rich. */
void rc_sim_inject(rc_sim_t *sim, uint8_t cylinder, float pulse_ms, float dead_time_ms,
                   float flow_cc_per_s, float fuel_density_gasoline, float fuel_pressure_kpa);

/** The ECU commanded a spark for a cylinder at `angle_deg`.  Returns the knock margin. */
float rc_sim_spark(rc_sim_t *sim, uint8_t cylinder, float angle_deg, float dwell_ms,
                   float iat_c, float load_pct);

#ifdef __cplusplus
}
#endif
#endif /* RC_SIM_H */
