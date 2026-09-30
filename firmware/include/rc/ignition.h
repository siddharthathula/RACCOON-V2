/* RACCOON V2 - ignition, dwell and knock.
 *
 * Hard rules (docs/IGNITION_SPEC.md, docs/KNOCK control section 34):
 *   I1  The command (degrees BTDC) is separated from the driver topology.  The firmware
 *       does not assume CDI compatibility and never does.
 *   I2  Corrections are summed in a fixed, documented order, and the user trim is
 *       applied BEFORE the hard envelope, so no combination of trims can exceed it.
 *   I3  Dwell is bounded by software AND by an independent hardware max-dwell timer that
 *       survives a halted CPU.
 *   I4  A per-cylinder cut is a scheduler suppression, never a "0 degrees" command.
 *   I5  Knock retard releases at a bounded rate; it never snaps back to full advance.
 */
#ifndef RC_IGNITION_H
#define RC_IGNITION_H

#include "cal_types.h"
#include "rc/engine.h"
#include "rc/rc_math.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float rpm;
    float load;
    float iat_c;
    float ect_c;
    float baro_kpa;
    float battery_v;
    float ethanol_fraction;
    uint8_t gear;
    float tps_pct;
    float user_trim_deg;
    uint8_t mode;
    float dt_s;
    bool crank_only;
} rc_ignition_input_t;

typedef struct {
    float base_deg[RC_MAX_CYLINDERS];
    float correction_deg;
    float final_deg[RC_MAX_CYLINDERS];
    float dwell_ms[RC_MAX_CYLINDERS];
    float knock_retard_deg[RC_MAX_CYLINDERS];
    uint8_t cut[RC_MAX_CYLINDERS];
    bool envelope_limited;
    float envelope_min_deg;
    float envelope_max_deg;
} rc_ignition_output_t;

/** Per-cylinder knock state, which must persist between control cycles. */
typedef struct {
    float retard_deg[RC_MAX_CYLINDERS];
    uint8_t event_count[RC_MAX_CYLINDERS];
    float window_start_deg10[RC_MAX_CYLINDERS];
    bool window_open[RC_MAX_CYLINDERS];
    float noise_floor[RC_MAX_CYLINDERS];
    uint32_t knock_events;
} rc_knock_state_t;

typedef struct {
    float retard_deg[RC_MAX_CYLINDERS];
    float noise_floor[RC_MAX_CYLINDERS];
    bool active[RC_MAX_CYLINDERS];
} rc_knock_output_t;

void rc_ignition_init(rc_knock_state_t *state);

/**
 * Compute the ignition command for every cylinder.
 *
 * The hard envelope is applied here, on the command, after every correction.  It is the
 * last thing that touches the value, which is what makes "the envelope always wins" a
 * structural property rather than a convention (I2).
 */
void rc_ignition_update(const rc_cal_t *cal, const rc_engine_t *engine,
                        const rc_knock_output_t *knock, const rc_ignition_input_t *in,
                        const bool *external_cut, rc_ignition_output_t *out);

/** Dwell for one cylinder, bounded by the software limits and the hardware ceiling. */
float rc_ignition_dwell_ms(const rc_cal_t *cal, float battery_v, float rpm);

/** Correction stack, in the documented order. Exposed so a log can show the arithmetic. */
float rc_ignition_correction_deg(const rc_cal_t *cal, const rc_ignition_input_t *in);

/** Feed a knock sample for a cylinder, inside or outside its window. */
void rc_knock_update(rc_knock_state_t *state, const rc_cal_t *cal, uint8_t cylinder,
                     float sample, float window_open_fraction);

/** Release knock retard at the calibrated rate (I5). */
void rc_knock_decay(rc_knock_state_t *state, const rc_cal_t *cal, float dt_s);

/** Render the current knock state for the ignition pipeline. */
void rc_knock_publish(const rc_knock_state_t *state, const rc_cal_t *cal, rc_knock_output_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RC_IGNITION_H */
