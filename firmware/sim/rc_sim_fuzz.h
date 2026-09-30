/* RACCOON V2 - simulator fuzzer.  See rc_sim_fuzz.c. */
#ifndef RC_SIM_FUZZ_H
#define RC_SIM_FUZZ_H

#include "rc_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What one fuzz case found.  Recorded rather than printed, so a failure can be replayed
 *  from the seed alone. */
typedef struct {
    rc_sim_faults_t faults;
    float fault_rate;
    bool non_finite;
    uint32_t cycles;
    uint32_t events;
    uint32_t dropped;
    float peak_rpm;
    uint32_t seed;
} rc_harness_fuzz_state_t;

/** Run one case.  Deterministic in `seed`: the same seed always produces the same run,
 *  which is what makes a finding reproducible rather than a rumour. */
void fuzz_run_case(uint32_t seed, float fault_rate, rc_harness_fuzz_state_t *fs);

#ifdef __cplusplus
}
#endif
#endif /* RC_SIM_FUZZ_H */
