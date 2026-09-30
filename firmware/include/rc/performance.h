/* RACCOON V2 - performance features as torque requesters.
 *
 * Launch, quickshift, traction control, engine braking, pit limiter and drag mode all live
 * here, and all of them do the same thing: they *request* reduced torque authority from
 * the torque manager.  None of them may touch the final fuel or ignition command, and
 * none of them can bypass the safety envelope (spec sections 41-47, 111, 112).
 */
#ifndef RC_PERFORMANCE_H
#define RC_PERFORMANCE_H

#include "cal_types.h"
#include "rc/rc_math.h"
#include "rc/rc_types.h"
#include "rc/torque.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RC_PERF_IDLE = 0,
    RC_PERF_LAUNCH_ARMED = 1,
    RC_PERF_LAUNCH_ACTIVE = 2,
    RC_PERF_SHIFT = 3,
    RC_PERF_TRACTION = 4,
    RC_PERF_ENGINE_BRAKING = 5,
    RC_PERF_PIT_LIMIT = 6,
    RC_PERF_DONE = 7
} rc_perf_state_t;

typedef struct {
    bool clutch_pressed;
    bool brake_pressed;
    bool launch_requested;
    bool shift_requested;
    bool pit_requested;
    bool throttle_closed;
    uint8_t gear;
    float rpm;
    float speed_kph;
    float front_speed_kph;
    float rear_speed_kph;
    float tps_pct;
    float dt_s;
    uint8_t mode;
} rc_perf_input_t;

typedef struct {
    rc_perf_state_t state;
    float launch_torque_scale;
    float shift_torque_scale;
    float traction_torque_scale;
    float slip_percent;
    uint32_t shift_events;
    uint32_t launch_events;
    uint32_t traction_events;
    float engine_braking_retard_deg;
    bool wheelie_mitigating;
} rc_perf_output_t;

typedef struct {
    rc_perf_state_t state;
    uint32_t state_since_us;
    uint32_t shift_until_us;
    uint32_t launch_until_us;
    bool launch_latched;
    bool pit_latched;
    /** True while a shift cut/blip window is in progress.  Needed because a zero
     *  timestamp is indistinguishable from "no shift has happened yet". */
    bool shift_active;
} rc_perf_state_store_t;

void rc_perf_init(rc_perf_state_store_t *store);

/**
 * Evaluate the performance features and submit their torque requests.
 *
 * The output is the *request set*, not a command.  control.c then hands the whole
 * request vector to the torque manager, which arbitrates.
 */
void rc_perf_update(rc_perf_state_store_t *store, const rc_cal_t *cal, rc_torque_manager_t *tm,
                    const rc_perf_input_t *in, uint32_t now_us, rc_perf_output_t *out);

/** Slip as a percentage, guarded against a zero denominator. */
float rc_perf_slip_percent(float front_kph, float rear_kph);

#ifdef __cplusplus
}
#endif
#endif /* RC_PERFORMANCE_H */
