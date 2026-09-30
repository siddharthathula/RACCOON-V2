/* RACCOON V2 - runtime calibration validation: per-item bounds and cross-item rules.
 *
 * This is the ECU's authoritative gate.  It runs on the *staged* image before it is
 * committed, and again on the *active* image at boot, because the same code must be able
 * to answer "is this configuration armable?" in the GUI and in the firmware from one
 * implementation.
 *
 * Two failure styles, deliberately distinguished:
 *   - a *violation* names a rule and is shown to the tuner with a remedy
 *   - a *warning* is surfaced in diagnostics and in the log header, never blocking
 */
#include "rc/cal_validate.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/engine.h"
#include "rc/rc_math.h"
#include "rc/rc_rules.h"

/* --------------------------------------------------------------- per-item */

uint32_t rc_cal_validate_items(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity)
{
    if (cal == NULL) {
        return 0u;
    }
    uint32_t count = 0u;
    /* Every item is checked, always.  `capacity` limits only how much DETAIL is written
     * back - never whether a check happens.  Putting capacity in the loop condition means
     * a caller that asks for "just tell me yes or no" (faults == NULL, capacity == 0)
     * silently skips every check and is told the calibration is fine. */
    for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
        const rc_cal_desc_t *desc = &rc_cal_desc[i];
        bool ok = true;
        float engineering = 0.0f;

        if (desc->type == RC_CAL_T_AXIS1D) {
            /* Every breakpoint must lie inside the axis's own range. The ORDERING is not
             * checked here: that is a relationship between breakpoints rather than a
             * property of one, so it is a cross-item rule (axis_breakpoints_increase) and
             * carries its own explanation. Reporting it here would blame a single item for
             * something two of them did. */
            const size_t points = (size_t)desc->size / desc->cell_bytes;
            for (size_t c = 0u; c < points; ++c) {
                if (rc_cal_axis_point(cal, desc->id, (uint16_t)c, &engineering) == false) {
                    ok = false;
                    break;
                }
                if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
                    ok = false;
                    break;
                }
            }
        } else if (desc->type == RC_CAL_T_TABLE2D) {
            /* Every cell must lie inside the declared cell range, and the generator has
             * already proved the cell storage type cannot overflow, so the only real risk
             * is a corrupted or out-of-schema image. */
            const size_t cells = (size_t)desc->alloc_rows * (size_t)desc->alloc_cols;
            for (size_t c = 0u; c < cells; ++c) {
                if (rc_cal_table_cell(cal, desc->id, (uint16_t)c, &engineering) == false) {
                    ok = false;
                    break;
                }
                if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
                    ok = false;
                    break;
                }
            }
        } else {
            if (rc_cal_get_float(cal, desc->id, &engineering) == false) {
                ok = false;
            } else if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
                ok = false;
            }
        }

        if (!ok) {
            if (faults != NULL && count < capacity) {
                faults[count].rule_index = RC_RULE_INDEX_NONE; /* an item fault, not a rule */
                faults[count].id = desc->id;
                faults[count].name = desc->name;
                faults[count].value = engineering;
                faults[count].severity = RC_SEVERITY_BLOCKING;
            }
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------ cross-item */

/* The rule prose, in the same order as tools/codegen/rc_rules.py.  The generated table
 * firmware/generated/cal_rules.h carries the names; this carries the wording, because a
 * refusal a tuner cannot act on is a bug. */
#include "cal_rules.h"

static const char *const k_rule_text[RC_GENERATED_RULE_COUNT] = {
        
            "the rev limit must be below the design maximum speed",
        
            "the overspeed fault threshold must be above the rev limit",
        
            "the idle target must sit well below the rev limit, or the limiter chatters",
        
            "the software dwell maximum must not exceed the hardware dwell ceiling",
        
            "the dwell table can command more than the hardware ceiling; those cells would be truncated by the max-dwell timer and the remaining energy dumped in the IGBT",
        
            "the minimum dwell must be below the maximum dwell",
        
            "the ignition table can hold a value outside the hard ignition envelope, so a cell would be silently clamped",
        
            "the target lambda table can command a mixture the lean or rich hard limit will refuse",
        
            "the boost target table can command more than the design maximum",
        
            "the VVT target table exceeds physically available cam travel",
        
            "the VVT duty limits must be ordered and inside 0 to 100 percent",
        
            "the injector duty ceiling must leave headroom below 100 percent",
        
            "sequential injection requires a cam phase source; the ECU will not fabricate one",
        
            "sequential injection requires a verified TDC; confirm it with the timing light or the TDC tool before arming",
        
            "sequential injection requires a verified cam phase",
        
            "the firing order must be a permutation of the cylinder indices",
        
            "two throttle channels are declared but DBW is off; a cable throttle does not have two sensors, so this is a wiring mistake or a mislabelled DBW",
        
            "the minimum plausible tooth period must be below the maximum",
        
            "the overspeed period must be above the minimum period, otherwise every legal edge would be declared an overspeed fault",
        
            "knock control is enabled with no knock sensor fitted",
        
            "the lambda strategy requires a channel that is not fitted",
        
            "traction control needs two wheel speed channels; slip cannot be computed from one",
        
            "electronic throttle requires dual-channel pedal and throttle sensing",
        
            "a fitted kill switch must have a fault behaviour that inhibits the engine; 'ignore' would allow engine control with a broken kill switch",
        
            "the mode bitfield must declare all ten race modes",
        
            "the mode switch coast time must be between 0 and 10000 ms",
        
            "a safety item is marked UNVERIFIED but cites a source",

            "axis breakpoints must strictly increase; a repeated or inverted pair makes the "
            "table it belongs to unsearchable, so the lookup returns a value from whichever "
            "cell it happened to land in",
};

uint16_t rc_rule_count(void)
{
    return RC_GENERATED_RULE_COUNT;
}

const char *rc_rule_name(uint16_t index)
{
    return (index < RC_GENERATED_RULE_COUNT) ? rc_rule_names[index] : "invalid";
}

const char *rc_rule_text(uint16_t rule_index)
{
    return (rule_index < RC_GENERATED_RULE_COUNT) ? k_rule_text[rule_index]
                                                  : "a calibration item is outside its declared bounds";
}

static void add_fault(rc_cal_fault_t *faults, uint32_t *count, uint32_t capacity,
                      uint16_t rule_index, uint16_t id, const char *name, float value,
                      rc_severity_t severity)
{
    if (faults == NULL || *count >= capacity) {
        (*count)++;
        return;
    }
    faults[*count].rule_index = rule_index;
    faults[*count].id = id;
    faults[*count].name = name;
    faults[*count].value = value;
    faults[*count].severity = severity;
    (*count)++;
}

uint32_t rc_cal_validate_rules(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity)
{
    if (cal == NULL) {
        return 0u;
    }
    uint32_t count = 0u;

/* Each rule reports the calibration id it is about, so the tuner can jump straight to
 * the offending value rather than being told only that "a rule was broken". */
#define RC_RULE(rule_index, condition, id, subject, value_expr)                       \
    do {                                                                              \
        if (!(condition)) {                                                           \
            add_fault(faults, &count, capacity, (uint16_t)(rule_index),                \
                      (uint16_t)(id), subject, (float)(value_expr),                   \
                      RC_SEVERITY_BLOCKING);                                          \
        }                                                                             \
    } while (0)

    /* Every axis strictly increasing. Looped over the descriptor rather than listing
     * sixteen ids, so an axis added to the schema is covered the day it is declared instead
     * of the day somebody remembers. The first offending axis is reported and the loop
     * stops: naming sixteen of them at once helps nobody, and the operator fixes them one
     * at a time anyway. */
    for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
        const rc_cal_desc_t *desc = &rc_cal_desc[i];
        if (desc->type != RC_CAL_T_AXIS1D) {
            continue;
        }
        if (rc_cal_axis_is_monotonic(cal, desc->id)) {
            continue;
        }
        /* Report the index of the first pair that is not increasing, because that is what
         * the operator needs to find the breakpoint they have to move. */
        uint16_t broken = 0u;
        float previous = 0.0f;
        float current = 0.0f;
        const uint8_t points = (uint8_t)((size_t)desc->size / (size_t)desc->cell_bytes);
        for (uint8_t p = 1u; p < points; ++p) {
            if (rc_cal_axis_point(cal, desc->id, (uint16_t)(p - 1u), &previous)
                && rc_cal_axis_point(cal, desc->id, p, &current)
                && !(current > previous)) {
                broken = p;
                break;
            }
        }
        RC_RULE(RC_RULE_INDEX_AXIS_BREAKPOINTS_INCREASE, false, desc->id, desc->name,
                (float)broken);
        break;
    }

    /* rpm_limit < max_rpm, and the idle target well below the limiter. */
    RC_RULE(RC_RULE_INDEX_RPM_LIMIT_BELOW_MAX_RPM, 
            rc_cal_get_safety_rpm_limit(cal) < rc_cal_get_engine_max_rpm(cal), RC_CAL_ID_SAFETY_RPM_LIMIT, "safety_rpm_limit", rc_cal_get_safety_rpm_limit(cal));
    RC_RULE(RC_RULE_INDEX_OVERSPEED_ABOVE_RPM_LIMIT, 
            rc_cal_get_safety_overspeed_rpm(cal) > rc_cal_get_safety_rpm_limit(cal), RC_CAL_ID_SAFETY_OVERSPEED_RPM, "safety_overspeed_rpm", rc_cal_get_safety_overspeed_rpm(cal));
    RC_RULE(RC_RULE_INDEX_IDLE_BELOW_RPM_LIMIT, 
            rc_cal_get_engine_idle_rpm_target(cal)
                < (rc_cal_get_safety_rpm_limit(cal) * 0.9f), RC_CAL_ID_ENGINE_IDLE_RPM_TARGET, "engine_idle_rpm_target", rc_cal_get_engine_idle_rpm_target(cal));

    /* Dwell: software maximum inside the hardware ceiling, and the table cannot ask for
     * more than the hardware will deliver. */
    RC_RULE(RC_RULE_INDEX_DWELL_MAX_WITHIN_HARDWARE, 
            rc_cal_get_ignition_dwell_max_ms(cal)
                <= rc_cal_get_hardware_ignition_max_dwell_ms(cal), RC_CAL_ID_IGNITION_DWELL_MAX_MS, "ignition_dwell_max_ms", rc_cal_get_ignition_dwell_max_ms(cal));
    RC_RULE(RC_RULE_INDEX_DWELL_SOFT_LIMITS_ORDERED, 
            rc_cal_get_ignition_dwell_min_ms(cal) < rc_cal_get_ignition_dwell_max_ms(cal), RC_CAL_ID_IGNITION_DWELL_MIN_MS, "ignition_dwell_min_ms", rc_cal_get_ignition_dwell_min_ms(cal));
    {
        float lo = 0.0f;
        float hi = 0.0f;
        const bool have = rc_cal_item_range(cal, RC_CAL_ID_IGNITION_DWELL_TABLE, &lo, &hi);
        RC_RULE(RC_RULE_INDEX_DWELL_TABLE_WITHIN_HARDWARE_CEILING, 
                (!have) || (hi <= rc_cal_get_hardware_ignition_max_dwell_ms(cal)), RC_CAL_ID_IGNITION_DWELL_TABLE, "dwell_table", hi);
    }

    /* Ignition table inside the hard envelope. */
    {
        float lo = 0.0f;
        float hi = 0.0f;
        const bool have = rc_cal_item_range(cal, RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, &lo, &hi);
        RC_RULE(RC_RULE_INDEX_IGNITION_TABLE_WITHIN_ENVELOPE, 
                (!have) || ((lo >= rc_cal_get_strategy_ignition_hard_min_deg(cal))
                            && (hi <= rc_cal_get_strategy_ignition_hard_max_deg(cal))), RC_CAL_ID_IGNITION_IGNITION_BASE_TABLE, "ignition_base_table", hi);
    }

    /* Lambda table inside the lean and rich hard limits. */
    {
        float lo = 0.0f;
        float hi = 0.0f;
        const bool have = rc_cal_item_range(cal, RC_CAL_ID_FUEL_TARGET_LAMBDA_TABLE, &lo, &hi);
        RC_RULE(RC_RULE_INDEX_LAMBDA_TABLE_WITHIN_SAFETY, 
                (!have) || ((lo >= rc_cal_get_safety_rich_lambda_limit(cal))
                            && (hi <= rc_cal_get_safety_lean_lambda_limit(cal))), RC_CAL_ID_FUEL_TARGET_LAMBDA_TABLE, "target_lambda_table", hi);
    }

    /* Boost target below the design maximum, emergency trip above it. */
    {
        float lo = 0.0f;
        float hi = 0.0f;
        const bool have = rc_cal_item_range(cal, RC_CAL_ID_BOOST_BOOST_TARGET_TABLE, &lo, &hi);
        RC_RULE(RC_RULE_INDEX_BOOST_TABLE_WITHIN_LIMIT, 
                (!have) || ((hi <= rc_cal_get_safety_boost_max_kpa(cal))
                            && (rc_cal_get_boost_boost_overboost_limit_kpa(cal)
                                >= rc_cal_get_safety_boost_max_kpa(cal))), RC_CAL_ID_BOOST_BOOST_TARGET_TABLE, "boost_target_table", hi);
    }

    /* VVT: cam travel and duty ordering. */
    {
        float lo = 0.0f;
        float hi = 0.0f;
        const bool have = rc_cal_item_range(cal, RC_CAL_ID_VVT_VVT_TARGET_TABLE, &lo, &hi);
        RC_RULE(RC_RULE_INDEX_VVT_TABLE_BOUNDED, (!have) || (rc_absf(lo) <= 60.0f && rc_absf(hi) <= 60.0f), RC_CAL_ID_VVT_VVT_TARGET_TABLE, "vvt_target_table", hi);
    }
    RC_RULE(RC_RULE_INDEX_VVT_DUTY_LIMITS_ORDERED, 
            (rc_cal_get_vvt_vvt_min_duty_pct(cal) >= 0.0f)
                && (rc_cal_get_vvt_vvt_min_duty_pct(cal)
                    < rc_cal_get_vvt_vvt_max_duty_pct(cal))
                && (rc_cal_get_vvt_vvt_max_duty_pct(cal) <= 100.0f), RC_CAL_ID_VVT_VVT_MIN_DUTY_PCT, "vvt_min_duty_pct", rc_cal_get_vvt_vvt_max_duty_pct(cal));

    /* Injector duty headroom. */
    RC_RULE(RC_RULE_INDEX_INJECTOR_DUTY_HEADROOM, 
            rc_cal_get_safety_injector_duty_max_pct(cal) < 100.0f, RC_CAL_ID_SAFETY_INJECTOR_DUTY_MAX_PCT, "safety_injector_duty_max_pct", rc_cal_get_safety_injector_duty_max_pct(cal));

    /* Firing order is a permutation. */
    {
        const uint8_t order[4] = {
            cal->engine_firing_order_cyl0, cal->engine_firing_order_cyl1,
            cal->engine_firing_order_cyl2, cal->engine_firing_order_cyl3
        };
        uint8_t seen = 0u;
        for (uint8_t i = 0u; i < 4u; ++i) {
            if (order[i] < 4u) {
                seen = (uint8_t)(seen | (1u << order[i]));
            }
        }
        RC_RULE(RC_RULE_INDEX_FIRING_ORDER_PERMUTATION, seen == 0x0Fu, RC_CAL_ID_ENGINE_FIRING_ORDER_CYL0, "firing_order_cyl0", 0.0f);
    }

    /* Strategy implies hardware. */
    {
        const uint8_t injection = cal->strategy_injection_strategy;
        const bool sequential = (injection == 2u || injection == 3u);
        RC_RULE(RC_RULE_INDEX_SEQUENTIAL_REQUIRES_CAM, 
                !sequential || (cal->trigger_profile_cam != 0u), RC_CAL_ID_STRATEGY_INJECTION_STRATEGY, "strategy_injection_strategy", (float)injection);
        RC_RULE(RC_RULE_INDEX_SEQUENTIAL_REQUIRES_VERIFIED_TDC, 
                !sequential || (cal->strategy_tdc_verified != 0u), RC_CAL_ID_STRATEGY_TDC_VERIFIED, "strategy_tdc_verified", (float)cal->strategy_tdc_verified);
        RC_RULE(RC_RULE_INDEX_SEQUENTIAL_REQUIRES_VERIFIED_CAM, 
                !sequential || (cal->strategy_cam_phase_verified != 0u), RC_CAL_ID_STRATEGY_CAM_PHASE_VERIFIED, "strategy_cam_phase_verified", (float)cal->strategy_cam_phase_verified);
        RC_RULE(RC_RULE_INDEX_THROTTLE_DUAL_NEEDS_DBW, 
                (cal->strategy_throttle_channel_count < 2u) || (cal->strategy_dbw_strategy != 0u), RC_CAL_ID_STRATEGY_THROTTLE_CHANNEL_COUNT, "strategy_throttle_channel_count", (float)cal->strategy_throttle_channel_count);
    }

    /* Trigger period bounds. */
    RC_RULE(RC_RULE_INDEX_MIN_PERIOD_BELOW_MAX_PERIOD, 
            cal->trigger_min_period_us < cal->trigger_max_period_us, RC_CAL_ID_TRIGGER_MIN_PERIOD_US, "trigger_min_period_us", (float)cal->trigger_min_period_us);
    RC_RULE(RC_RULE_INDEX_OVERSPEED_PERIOD_ABOVE_MIN, 
            cal->trigger_overspeed_period_us > cal->trigger_min_period_us, RC_CAL_ID_TRIGGER_OVERSPEED_PERIOD_US, "trigger_overspeed_period_us", (float)cal->trigger_overspeed_period_us);

    /* Feature implies hardware. */
    RC_RULE(RC_RULE_INDEX_KNOCK_REQUIRES_SENSOR, 
            (cal->knock_knock_enabled == 0u) || (cal->strategy_knock_sensor_count > 0u), RC_CAL_ID_STRATEGY_KNOCK_SENSOR_COUNT, "strategy_knock_sensor_count", (float)cal->strategy_knock_sensor_count);
    RC_RULE(RC_RULE_INDEX_LAMBDA_STRATEGY_REQUIRES_SENSOR, 
            (cal->strategy_lambda_strategy <= 1u) || (cal->strategy_lambda_channel_count > 0u), RC_CAL_ID_STRATEGY_LAMBDA_CHANNEL_COUNT, "strategy_lambda_channel_count", (float)cal->strategy_lambda_channel_count);
    RC_RULE(RC_RULE_INDEX_TRACTION_REQUIRES_WHEEL_SPEED, 
            (cal->traction_traction_enabled == 0u) || (cal->strategy_wheel_speed_channel_count >= 2u), RC_CAL_ID_STRATEGY_WHEEL_SPEED_CHANNEL_COUNT, "strategy_wheel_speed_channel_count", (float)cal->strategy_wheel_speed_channel_count);
    RC_RULE(RC_RULE_INDEX_DBW_REQUIRES_DUAL_CHANNEL, 
            (cal->strategy_dbw_strategy == 0u)
                || ((cal->strategy_pedal_channel_count >= 2u)
                    && (cal->strategy_throttle_channel_count >= 2u)), RC_CAL_ID_STRATEGY_PEDAL_CHANNEL_COUNT, "strategy_pedal_channel_count", (float)cal->strategy_pedal_channel_count);
    RC_RULE(RC_RULE_INDEX_KILL_SWITCH_BLOCKS_ARM, 
            (cal->switches_kill_switch_present == 0u)
                || (cal->switches_kill_switch_fault_behaviour != 0u), RC_CAL_ID_SWITCHES_KILL_SWITCH_FAULT_BEHAVIOUR, "switches_kill_switch_fault_behaviour", (float)cal->switches_kill_switch_fault_behaviour);

    /* Modes. */
    RC_RULE(RC_RULE_INDEX_MODES_BITFIELD_COVERS_ALL, 
            (float)cal->modes_mode_count <= 10.0f, RC_CAL_ID_MODES_MODE_COUNT, "modes_mode_count", (float)cal->modes_mode_count);
    RC_RULE(RC_RULE_INDEX_MODE_SWITCH_COAST_BOUNDED, 
            rc_cal_get_modes_mode_switch_coast_ms(cal) <= 10000.0f, RC_CAL_ID_MODES_MODE_SWITCH_COAST_MS, "modes_mode_switch_coast_ms", rc_cal_get_modes_mode_switch_coast_ms(cal));

#undef RC_RULE
    return count;
}

bool rc_cal_validate(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity,
                     uint32_t *blocking_count)
{
    /* Both phases always run, and the counts they return are the true number of faults
     * found rather than the number that fitted in the caller's buffer.  The detail buffer
     * is written only up to its capacity.  This is what lets the same validator answer
     * "is this safe to run?" for a boot-time check and "show me everything wrong" for the
     * tuner without the two disagreeing. */
    const uint32_t item_faults = rc_cal_validate_items(cal, faults, capacity);
    uint32_t found = item_faults;
    rc_cal_fault_t *rule_slot = NULL;
    uint32_t rule_capacity = 0u;
    if (faults != NULL && item_faults < capacity) {
        rule_slot = &faults[item_faults];
        rule_capacity = capacity - item_faults;
    }
    found += rc_cal_validate_rules(cal, rule_slot, rule_capacity);
    if (blocking_count != NULL) {
        *blocking_count = found;
    }
    return (found == 0u);
}

const char *rc_cal_severity_name(rc_severity_t severity)
{
    switch (severity) {
        case RC_SEVERITY_INFO: return "info";
        case RC_SEVERITY_WARNING: return "warning";
        case RC_SEVERITY_BLOCKING:
        default: return "blocking";
    }
}
