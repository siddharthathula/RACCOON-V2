/* RACCOON V2 - universal engine model. See engine.h for the design note. */
#include "rc/engine.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"

#define RC_PI_F 3.14159265358979323846f

float rc_engine_displacement_cc(float bore_mm, float stroke_mm, uint8_t cylinders)
{
    if (bore_mm <= 0.0f || stroke_mm <= 0.0f || cylinders == 0u) {
        return 0.0f;
    }
    const float bore_cm = bore_mm * 0.1f;
    const float stroke_cm = stroke_mm * 0.1f;
    return (RC_PI_F / 4.0f) * bore_cm * bore_cm * stroke_cm * (float)cylinders;
}

float rc_engine_clearance_cc(float swept_cc, float compression_ratio)
{
    if (swept_cc <= 0.0f || compression_ratio <= 1.0f) {
        return 0.0f;
    }
    return swept_cc / (compression_ratio - 1.0f);
}

bool rc_engine_build(const rc_cal_t *cal, rc_engine_t *out, const char **reason)
{
    static const rc_injection_strategy_t k_inj[] = {
        RC_INJ_BATCH, RC_INJ_SEMI_SEQUENTIAL, RC_INJ_SEQUENTIAL, RC_INJ_STAGED
    };
    static const rc_ignition_strategy_t k_ign[] = {
        RC_IGN_SEQUENTIAL, RC_IGN_WASTED_SPARK, RC_IGN_PAIRED, RC_IGN_SMART_COIL, RC_IGN_DUMB_COIL_EXTERNAL
    };

    const char *why = NULL;
    if (out == NULL || cal == NULL) {
        if (reason != NULL) {
            *reason = "null argument";
        }
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* ------------------------------------------------------ cylinder count */
    const uint8_t cylinders = rc_cal_get_engine_cylinder_count(cal);
    if (cylinders < 1u || cylinders > RC_MAX_CYLINDERS) {
        why = "cylinder_count must be 1..4";
        goto fail;
    }

    /* ---------------------------------------------------------- cycle type */
    const uint8_t cycle = cal->engine_cycle_teeth;
    if (cycle != 1u) {
        /* Value 0 is the two-stroke, which this product does not support: sequential
         * injection over a 360 degree cycle is a different, unimplemented model. */
        why = "only the 720 degree four-stroke cycle is implemented";
        goto fail;
    }

    /* ------------------------------------------------------------ geometry */
    out->bore_mm = rc_cal_get_engine_bore_mm(cal);
    out->stroke_mm = rc_cal_get_engine_stroke_mm(cal);
    out->compression_ratio = rc_cal_get_engine_compression_ratio(cal);
    out->rod_ratio = rc_cal_get_engine_rod_ratio(cal);
    if (!(out->bore_mm > 0.0f) || !isfinite(out->bore_mm)) {
        why = "bore_mm must be positive and finite";
        goto fail;
    }
    if (!(out->stroke_mm > 0.0f) || !isfinite(out->stroke_mm)) {
        why = "stroke_mm must be positive and finite";
        goto fail;
    }
    if (!(out->compression_ratio > 1.0f) || !isfinite(out->compression_ratio)) {
        why = "compression_ratio must exceed 1 and be finite";
        goto fail;
    }
    if (!(out->rod_ratio >= 1.0f) || !isfinite(out->rod_ratio)) {
        why = "rod_ratio must be at least 1 and finite";
        goto fail;
    }

    out->displacement_cc = rc_engine_displacement_cc(out->bore_mm, out->stroke_mm, cylinders);
    if (!(out->displacement_cc > 0.0f)) {
        why = "derived displacement is not positive - check bore, stroke and cylinder count";
        goto fail;
    }
    out->swept_cc = out->displacement_cc / (float)cylinders;
    out->clearance_cc = rc_engine_clearance_cc(out->swept_cc, out->compression_ratio);
    out->volumetric_cc_per_rev = out->displacement_cc * 0.5f; /* four-stroke */
    out->cycle_teeth = 720u;

    /* Cross-check against a supplied displacement when one is present. */
    const float stated = rc_cal_get_engine_displacement_cc(cal);
    if (stated > 0.0f) {
        const float derived = out->displacement_cc;
        const float error = fabsf(derived - stated) / stated;
        if (error > 0.02f) {
            why = "stated displacement disagrees with bore x stroke by more than 2 percent";
            goto fail;
        }
    }

    /* --------------------------------------------------------- crank layout */
    out->crank_layout = (rc_crank_layout_t)cal->engine_crank_layout;

    /* Firing angles: 720/cylinders apart, expressed from the layout. */
    switch (out->crank_layout) {
        case RC_CRANK_SINGLE:
        case RC_CRANK_PARALLEL_180:
        case RC_CRANK_BOXER_180:
        case RC_CRANK_PARALLEL_270:
        case RC_CRANK_INLINE_FOUR:
            /* The *firing interval* of a four-stroke is always 720/cylinders: the layout
             * decides the mechanical phase, the firing interval decides the timing. For a
             * 180 degree twin that is 360, for a 270 degree V-twin 240, for an inline
             * four 180. Identical maths, which is exactly why no engine-specific code
             * is needed. */
            break;
        case RC_CRANK_CUSTOM:
            break;
        default:
            why = "unknown crank layout";
            goto fail;
    }

    /* Firing order must be a permutation of 0..cylinders-1. */
    {
        uint8_t seen[RC_MAX_CYLINDERS] = {0u, 0u, 0u, 0u};
        const uint8_t order[RC_MAX_CYLINDERS] = {
            cal->engine_firing_order_cyl0,
            cal->engine_firing_order_cyl1,
            cal->engine_firing_order_cyl2,
            cal->engine_firing_order_cyl3,
        };
        /* Only the first `cylinders` entries are meaningful.  A twin leaves the last two
         * slots unused, and their shipped defaults must not be allowed to invalidate a
         * perfectly good twin configuration. */
        for (uint8_t i = 0u; i < cylinders; ++i) {
            if (order[i] >= cylinders) {
                why = "firing order references a cylinder index outside the cylinder count";
                goto fail;
            }
            if (seen[order[i]] != 0u) {
                why = "firing order is not a permutation: a cylinder appears twice";
                goto fail;
            }
            seen[order[i]] = 1u;
        }
        for (uint8_t i = 0u; i < cylinders; ++i) {
            out->firing_order[i] = order[i];
            out->fire_index_of[order[i]] = i;
        }
    }

    /* Custom firing angles, when the layout asks for them. */
    if (out->crank_layout == RC_CRANK_CUSTOM) {
        const float custom[RC_MAX_CYLINDERS] = {
            rc_cal_get_engine_custom_firing_angle_0(cal),
            rc_cal_get_engine_custom_firing_angle_1(cal),
            rc_cal_get_engine_custom_firing_angle_2(cal),
            rc_cal_get_engine_custom_firing_angle_3(cal),
        };
        for (uint8_t i = 0u; i < cylinders; ++i) {
            if (!(custom[i] >= 0.0f) || !(custom[i] < 720.0f)) {
                why = "custom firing angle must lie in [0,720) degrees";
                goto fail;
            }
            out->firing_angle_deg[i] = custom[i];
        }
    } else {
        const float interval = 720.0f / (float)cylinders;
        for (uint8_t i = 0u; i < cylinders; ++i) {
            out->firing_angle_deg[i] = interval * (float)i;
        }
    }

    /* --------------------------------------------------------- strategies */
    const uint8_t inj_raw = cal->strategy_injection_strategy;
    const uint8_t ign_raw = cal->ignition_ignition_strategy;
    if (inj_raw >= (sizeof(k_inj) / sizeof(k_inj[0]))) {
        why = "unknown injection strategy";
        goto fail;
    }
    if (ign_raw >= (sizeof(k_ign) / sizeof(k_ign[0]))) {
        why = "unknown ignition strategy";
        goto fail;
    }
    out->injection = k_inj[inj_raw];
    out->ignition = k_ign[ign_raw];

    out->cylinder_count = cylinders;
    if (reason != NULL) {
        *reason = NULL;
    }
    return true;

fail:
    memset(out, 0, sizeof(*out));
    if (reason != NULL) {
        *reason = (why != NULL) ? why : "invalid engine configuration";
    }
    return false;
}

float rc_engine_firing_angle_deg(const rc_engine_t *engine, uint8_t fire_index)
{
    if (engine == NULL || fire_index >= engine->cylinder_count) {
        return 0.0f;
    }
    return engine->firing_angle_deg[fire_index];
}

uint8_t rc_engine_injector_for(const rc_engine_t *engine, uint8_t cylinder)
{
    if (engine == NULL || cylinder >= engine->cylinder_count) {
        return 0xFFu;
    }
    switch (engine->injection) {
        case RC_INJ_SEQUENTIAL:
        case RC_INJ_STAGED:
            /* One injector per cylinder, driven at that cylinder's intake stroke. */
            return cylinder;
        case RC_INJ_SEMI_SEQUENTIAL:
            /* Pairs fire together; which pair depends on the cycle, so the scheduler is
             * told the pair index and alternates per cycle. */
            return cylinder;
        case RC_INJ_BATCH:
        default:
            return 0u;
    }
}

uint8_t rc_engine_ignition_channel_for(const rc_engine_t *engine, uint8_t cylinder)
{
    if (engine == NULL || cylinder >= engine->cylinder_count) {
        return 0xFFu;
    }
    switch (engine->ignition) {
        case RC_IGN_WASTED_SPARK:
            /* Pairs share a channel; the driver polarity is per coil. */
            return (uint8_t)(cylinder & 1u);
        case RC_IGN_PAIRED:
            return (uint8_t)(cylinder >> 1);
        case RC_IGN_SEQUENTIAL:
        case RC_IGN_SMART_COIL:
        case RC_IGN_DUMB_COIL_EXTERNAL:
        default:
            return cylinder;
    }
}

bool rc_engine_phase_source_valid(const rc_cal_t *cal)
{
    /* A phase source is either a cam signal, or a crank gap that fixes the 720 degree
     * cycle, and in both cases the phase must have been verified on the bench.  The
     * firmware never fabricates a phase source (spec section 12). */
    if (cal->trigger_profile_cam == 0u) {
        return false;
    }
    if (cal->trigger_teeth_per_rev == 2u) {
        return false; /* a cam wheel at 2 teeth per revolution is a distributor, not a phase reference */
    }
    return cal->strategy_cam_phase_verified != 0u;
}

bool rc_engine_strategy_valid(const rc_cal_t *cal, const char **reason)
{
    if (cal == NULL) {
        if (reason != NULL) {
            *reason = "null configuration";
        }
        return false;
    }
    const uint8_t inj = cal->strategy_injection_strategy;
    const bool needs_phase = (inj == (uint8_t)RC_INJ_SEQUENTIAL || inj == (uint8_t)RC_INJ_STAGED);
    if (needs_phase && !rc_engine_phase_source_valid(cal)) {
        if (reason != NULL) {
            *reason = "sequential injection requires a verified cam phase source; "
                      "no phase source may be fabricated";
        }
        return false;
    }
    if (reason != NULL) {
        *reason = NULL;
    }
    return true;
}
