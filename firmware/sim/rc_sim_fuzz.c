/* RACCOON V2 - simulator fuzzer.
 *
 * The scenarios assert behaviour.  This asserts only that the ECU survives being abused:
 * random rider inputs, random sensor faults, random tunes and random trigger waveforms,
 * for a long simulated time, with every published value checked for NaN and every loop
 * bounded.  It is here to find the crash, the hang and the NaN - the failures that a
 * reviewer cannot see by reading the code, because they need the right combination of
 * inputs and it is not obvious which combination that is.
 *
 * The randomness is seeded and reproducible.  A fuzzer that finds a bug you cannot
 * reproduce is a rumour, and a rumour does not get fixed.
 */
#include "rc_sim_fuzz.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "cal_types.h"
#include "rc/cal.h"
#include "rc_sim_harness.h"

/* A small deterministic PRNG (xorshift32).  Not cryptographic and does not need to be:
 * it needs to be reproducible, which is the whole point. */
typedef struct {
    uint32_t state;
} fuzz_rng_t;

static uint32_t rng_next(fuzz_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

static float rng_range(fuzz_rng_t *rng, float lo, float hi)
{
    const uint32_t r = rng_next(rng);
    /* 24 bits of mantissa is plenty and avoids the endpoint being unreachable. */
    const float unit = (float)(r >> 8) / 16777215.0f;
    return lo + (unit * (hi - lo));
}

static bool rng_chance(fuzz_rng_t *rng, float probability)
{
    return rng_range(rng, 0.0f, 1.0f) < probability;
}

static void fuzz_build_cal(rc_harness_t *h, fuzz_rng_t *rng, uint8_t cylinders, uint8_t teeth,
                           uint8_t missing, uint8_t cam)
{
    rc_cal_t cal = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_cylinder_count(&cal, cylinders);
    (void)rc_cal_set_engine_crank_layout(&cal, (uint8_t)rng_range(rng, 0.0f, 5.0f));
    (void)rc_cal_set_trigger_teeth(&cal, teeth);
    (void)rc_cal_set_trigger_missing_teeth(&cal, missing);
    (void)rc_cal_set_trigger_profile_cam(&cal, cam);
    (void)rc_cal_set_trigger_input_filter_level(&cal, (uint8_t)rng_range(rng, 0.0f, 3.0f));
    (void)rc_cal_set_trigger_sync_skip_revolutions(&cal, (uint8_t)rng_range(rng, 0.0f, 3.0f));
    (void)rc_cal_set_strategy_tdc_verified(&cal, rng_chance(rng, 0.8f));
    (void)rc_cal_set_strategy_cam_phase_verified(&cal, rng_chance(rng, 0.8f));
    (void)rc_cal_set_strategy_injection_strategy(&cal, (uint8_t)rng_range(rng, 0.0f, 3.0f));
    (void)rc_cal_set_idle_idle_strategy(&cal, (uint8_t)rng_range(rng, 0.0f, 4.0f));
    (void)rc_cal_set_fuel_injector_flow_cc_per_s(&cal, rng_range(rng, 1.0f, 40.0f));
    (void)rc_cal_set_fuel_injector_min_pulse_ms(&cal, rng_range(rng, 0.1f, 5.0f));
    (void)rc_cal_set_fuel_injector_max_pulse_ms(&cal, rng_range(rng, 5.0f, 60.0f));
    (void)rc_cal_set_safety_rpm_limit(&cal, rng_range(rng, 2000.0f, 12000.0f));
    (void)rc_cal_set_engine_max_rpm(&cal, rng_range(rng, 2000.0f, 14000.0f));
    (void)rc_cal_set_fuel_learner_enabled(&cal, rng_chance(rng, 0.5f));
    (void)rc_cal_set_strategy_lambda_strategy(&cal, (uint8_t)rng_range(rng, 0.0f, 4.0f));
    (void)rc_cal_set_launch_launch_enabled(&cal, rng_chance(rng, 0.3f));
    (void)rc_cal_set_traction_traction_enabled(&cal, rng_chance(rng, 0.3f));
    (void)rc_cal_set_pit_pit_limiter_enabled(&cal, rng_chance(rng, 0.3f));
    (void)rc_cal_set_shift_shift_blip_enabled(&cal, rng_chance(rng, 0.3f));
    (void)rc_cal_set_ebt_engine_braking_enabled(&cal, rng_chance(rng, 0.3f));
    rc_harness_set_cal(h, &cal);
}

static void fuzz_apply_faults(rc_harness_fuzz_state_t *fs, fuzz_rng_t *rng)
{
    const float p = fs->fault_rate;
    fs->faults.tps_stuck_low = rng_chance(rng, p);
    fs->faults.tps_stuck_high = !fs->faults.tps_stuck_low && rng_chance(rng, p);
    fs->faults.map_rail = rng_chance(rng, p * 0.5f);
    fs->faults.map_open = rng_chance(rng, p * 0.5f);
    fs->faults.ect_open = rng_chance(rng, p * 0.5f);
    fs->faults.oil_sensor_stuck = rng_chance(rng, p * 0.5f);
    fs->faults.lambda_noisy = rng_chance(rng, p);
    fs->faults.battery_v = (rng_chance(rng, p * 0.5f)) ? rng_range(rng, 5.0f, 16.0f) : 0.0f;
    fs->faults.injector_flow_scale = (rng_chance(rng, p * 0.5f)) ? rng_range(rng, 0.2f, 3.0f)
                                                                : 1.0f;
    fs->faults.trigger_noise = 0.0f;
}

static void fuzz_apply_rider(rc_harness_t *h, fuzz_rng_t *rng, float seconds)
{
    /* Inputs move on their own timescale, not every step: a throttle that changes every
     * 100 us is not a rider, and it would never let the closed loop settle into any state
     * worth finding a bug in. */
    const float move = seconds * 8.0f;
    h->rider.throttle_pct = rc_clampf(h->rider.throttle_pct + rng_range(rng, -move, move), 0.0f,
                                       100.0f);
    h->rider.brake_pct = rc_clampf(h->rider.brake_pct + rng_range(rng, -move, move), 0.0f,
                                   100.0f);
    h->rider.clutch_engaged = rng_chance(rng, 0.3f);
    h->rider.starter_engaged = rng_chance(rng, 0.2f);
    h->rider.gear = (uint8_t)rng_range(rng, 0.0f, 4.0f);
    h->rider.launch_button = rng_chance(rng, 0.05f);
    h->rider.shift_up_requested = rng_chance(rng, 0.05f);
    h->rider.shift_down_requested = rng_chance(rng, 0.05f);
}

void fuzz_run_case(uint32_t seed, float fault_rate, rc_harness_fuzz_state_t *fs)
{
    fuzz_rng_t rng;
    rng.state = (seed == 0u) ? 0x1234567u : seed;

    rc_harness_t h;
    const uint8_t cylinders = (uint8_t)(1u + (rng_next(&rng) % 4u));
    const uint8_t teeth = (uint8_t)(1u + (rng_next(&rng) % 60u));
    const uint8_t missing = (uint8_t)(rng_next(&rng) % (teeth + 1u));
    const uint8_t cam = (uint8_t)(rng_next(&rng) % 4u);
    rc_harness_init(&h, rng_range(&rng, 50.0f, 500.0f), cylinders, teeth, missing, cam);

    fuzz_build_cal(&h, &rng, cylinders, teeth, missing, cam);
    if (fs == NULL) {
        static rc_harness_fuzz_state_t local;
        fs = &local;
    }
    memset(&fs->faults, 0, sizeof(fs->faults));
    fs->faults.injector_flow_scale = 1.0f;
    fs->fault_rate = fault_rate;
    fuzz_apply_faults(fs, &rng);

    /* Arm or do not, at random: half the interesting paths are the ones where arming
     * fails, and a fuzzer that always arms never reaches them. */
    rc_harness_set_armed(&h, rng_chance(&rng, 0.7f));

    const float segment = 0.05f; /* 50 ms between input moves */
    const float duration = 8.0f;
    for (float t = 0.0f; t < duration; t += segment) {
        h.faults = fs->faults;
        fuzz_apply_rider(&h, &rng, segment);
        if (rng_chance(&rng, 0.01f)) {
            rc_harness_set_armed(&h, !h.armed);
        }
        rc_harness_run(&h, segment, 0.0001f);
    }
    fs->non_finite = h.non_finite_seen;
    fs->cycles = h.cycles_run;
    fs->events = h.events_fired;
    fs->dropped = h.events_dropped;
    fs->peak_rpm = h.sim.peak_rpm;
    fs->seed = seed;
}
