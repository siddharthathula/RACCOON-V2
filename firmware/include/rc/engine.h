/* RACCOON V2 - universal engine model.
 *
 * An engine is DATA.  There is no per-engine code path anywhere in the control core:
 * the same functions serve a single, a parallel twin, a 270 degree V-twin, a triple and
 * an inline four, driven by the geometry and layout in the calibration
 * (docs spec sections 6, 7, 93; GT650_RESEARCH.md section 3).
 *
 * DESIGN-NOTE
 *   Requirements: PRODUCT_REQUIREMENTS R-UNIV; docs/ENGINE_CONTROL_SPEC.md 1, 2, 5.
 *   Public sources consulted: none for the mathematics.  The firing-angle and
 *   displacement derivations are standard four-stroke kinematics.
 *   INDEPENDENTLY AUTHORED.
 */
#ifndef RC_ENGINE_H
#define RC_ENGINE_H

#include "cal_types.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Crank arrangements (docs spec section 7). */
typedef enum {
    RC_CRANK_SINGLE = 0,
    RC_CRANK_PARALLEL_180 = 1,
    RC_CRANK_PARALLEL_270 = 2,
    RC_CRANK_INLINE_FOUR = 3,
    RC_CRANK_BOXER_180 = 4,
    RC_CRANK_CUSTOM = 5
} rc_crank_layout_t;

/** Injection firing modes (docs spec section 7). */
typedef enum {
    RC_INJ_BATCH = 0,
    RC_INJ_SEMI_SEQUENTIAL = 1,
    RC_INJ_SEQUENTIAL = 2,
    RC_INJ_STAGED = 3
} rc_injection_strategy_t;

/** Ignition firing modes. */
typedef enum {
    RC_IGN_SEQUENTIAL = 0,
    RC_IGN_WASTED_SPARK = 1,
    RC_IGN_PAIRED = 2,
    RC_IGN_SMART_COIL = 3,
    RC_IGN_DUMB_COIL_EXTERNAL = 4
} rc_ignition_strategy_t;

/** Derived engine description. Computed once at configuration load, never at runtime. */
typedef struct {
    uint8_t cylinder_count;
    uint16_t cycle_teeth;     /**< 720 for a four-stroke */
    rc_crank_layout_t crank_layout;
    rc_injection_strategy_t injection;
    rc_ignition_strategy_t ignition;

    float bore_mm;
    float stroke_mm;
    float compression_ratio;
    float rod_ratio;
    float displacement_cc;    /**< total */
    float swept_cc;           /**< per cylinder */
    float clearance_cc;       /**< derived from the compression ratio */
    float volumetric_cc_per_rev;/**< displacement / 2 for a four-stroke */

    /** Firing angle of each physical cylinder after its own TDC, degrees. */
    float firing_angle_deg[RC_MAX_CYLINDERS];
    /** Physical cylinder that fires at firing index i. */
    uint8_t firing_order[RC_MAX_CYLINDERS];
    /** Firing index of each physical cylinder. */
    uint8_t fire_index_of[RC_MAX_CYLINDERS];
} rc_engine_t;

/**
 * Build the derived engine description from calibration.
 *
 * @return true when the description is self-consistent.  On failure `out` is left
 *         zeroed and `reason` (if non-NULL) receives a short machine-readable string.
 *
 * Rejects: cylinder counts outside 1..4, non-finite or non-positive geometry, a firing
 * order that is not a permutation, and a geometry whose derived displacement disagrees
 * with a supplied displacement by more than 2 %.
 */
bool rc_engine_build(const rc_cal_t *cal, rc_engine_t *out, const char **reason);

/** Total displacement in cubic centimetres, derived from bore and stroke. */
float rc_engine_displacement_cc(float bore_mm, float stroke_mm, uint8_t cylinders);

/** Clearance volume from the compression ratio and swept volume, cm^3. */
float rc_engine_clearance_cc(float swept_cc, float compression_ratio);

/**
 * Absolute crank angle (0..720) of the given cylinder's power stroke TDC.
 *
 * Firing index 0 sits at the cycle origin; each subsequent firing index is offset by
 * 720/cylinders.  For a 180 degree parallel twin that is 360 degrees per fire, which is
 * the textbook answer; for a 270 degree V-twin, 240; for an inline four, 180.
 */
float rc_engine_firing_angle_deg(const rc_engine_t *engine, uint8_t fire_index);

/** Injector channel assigned to a physical cylinder, honouring the strategy. */
uint8_t rc_engine_injector_for(const rc_engine_t *engine, uint8_t cylinder);

/** Ignition channel assigned to a physical cylinder, honouring the strategy. */
uint8_t rc_engine_ignition_channel_for(const rc_engine_t *engine, uint8_t cylinder);

/** True when the configuration has a trustworthy cylinder phase source. */
bool rc_engine_phase_source_valid(const rc_cal_t *cal);

/** True when the strategy is physically valid for this engine. */
bool rc_engine_strategy_valid(const rc_cal_t *cal, const char **reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_ENGINE_H */
