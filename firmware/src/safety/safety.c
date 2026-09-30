/* RACCOON V2 - hard safety envelope. See safety.h and SAFETY_CASE.md. */
#include "rc/safety.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "rc/rc_math.h"

/* A hazard must persist before it latches, so a single noisy sample cannot shut the
 * engine down.  These are the design values, not measurements; UNK-*.

   lean_hold: long enough to ignore a transient rich/lean swing, short enough that a real
   lean condition still gets fuel cut before damage.
   oil_hold:  oil pressure is noisy at low speed by nature, so it is given a longer
   confirmation and a hysteresis recovery band (oil_pressure_recovery_bar). */
#define RC_LEAN_HOLD_US 250000u
#define RC_OIL_HOLD_US 400000u

void rc_safety_init(rc_safety_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->lean_hold_us = RC_LEAN_HOLD_US;
    state->oil_hold_us = RC_OIL_HOLD_US;
}

static rc_action_t worst(rc_action_t a, rc_action_t b)
{
    return (a >= b) ? a : b;
}

void rc_safety_evaluate(rc_safety_state_t *state, const rc_cal_t *cal, const rc_engine_t *engine,
                        const rc_safety_input_t *in, uint32_t now_us, rc_safety_output_t *out)
{
    if (state == NULL || cal == NULL || engine == NULL || in == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->torque_factor = 1.0f;
    out->action = RC_ACT_NONE;
    out->first_reason = NULL;

    /* The envelope's own ignition limits, narrowed - never widened - by engineering mode
     * is applied here as the ceiling. */
    out->ignition_min_deg = rc_cal_get_strategy_ignition_hard_min_deg(cal);
    out->ignition_max_deg = rc_cal_get_strategy_ignition_hard_max_deg(cal);

#define RC_TRIGGER(bit, reason_literal)                                    \
    do {                                                                   \
        if ((out->active & (bit)) == 0u) {                                \
            out->first_reason = (reason_literal);                          \
        }                                                                  \
        out->active |= (bit);                                              \
    } while (0)

    /* ---------------------------------------------------------- rev limiter */
    const float rpm_limit = rc_cal_get_safety_rpm_limit(cal);
    if (in->rpm >= rpm_limit) {
        RC_TRIGGER(RC_SAFETY_RPM, "rev limit");
        out->action = worst(out->action, RC_ACT_CUT);
        out->spark_cut = true;
    }
    const float overspeed = rc_cal_get_safety_overspeed_rpm(cal);
    if (in->rpm >= overspeed) {
        RC_TRIGGER(RC_SAFETY_OVERSPEED, "overspeed");
        out->action = worst(out->action, RC_ACT_SHUTDOWN);
        out->fuel_cut = true;
        out->spark_cut = true;
        out->torque_factor = 0.0f;
    }

    /* ------------------------------------------------------------- mixture */
    /* Losing the lambda while the tune is running closed loop IS a sensor fault, even
     * though no channel raised one: the closed-loop controller is now being asked to
     * correct against a measurement it does not have.  The mixture limit below correctly
     * ignores an invalid reading - but "ignore it" must not quietly mean "carry on as
     * though the closed loop were still working", because that is precisely the state in
     * which a real mixture excursion would go unnoticed. */
    const bool closed_loop = (rc_cal_get_strategy_lambda_strategy(cal) != 0u);
    const bool sensor_degraded = (in->lambda_valid == 0u) && closed_loop;

    /* Only a *valid* measurement can trip a mixture limit: a faulted lambda must not
     * produce a mixture cut, and equally must not be allowed to disable one. */
    if (in->lambda_valid != 0u) {
        const float lean_limit = rc_cal_get_safety_lean_lambda_limit(cal);
        const float rich_limit = rc_cal_get_safety_rich_lambda_limit(cal);
        if (in->lambda > lean_limit) {
            if (state->lean_latched == false) {
                if (state->lean_since_us == 0u) {
                    state->lean_since_us = now_us;
                }
                if (rc_ticks_forward(now_us, state->lean_since_us) >= state->lean_hold_us) {
                    state->lean_latched = true;
                }
            }
        } else {
            state->lean_since_us = 0u;
        }
        if (state->lean_latched != false) {
            RC_TRIGGER(RC_SAFETY_LEAN, "lean limit");
            out->action = worst(out->action, RC_ACT_CUT);
            out->fuel_cut = true;
            out->torque_factor = 0.0f;
        }
        if (in->lambda < rich_limit) {
            /* Over-fuelling is a fault, not a cut condition: the engine is rich but
             * running, and cutting fuel into a rich condition makes it worse. */
            RC_TRIGGER(RC_SAFETY_RICH, "rich limit");
            out->action = worst(out->action, RC_ACT_WARNING);
        }
    }

    /* ---------------------------------------------------------- temperature */
    if ((in->ect_valid != 0u) && (in->ect_c > rc_cal_get_safety_ect_max_c(cal))) {
        RC_TRIGGER(RC_SAFETY_ECT, "engine temperature");
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.5f);
    }
    if ((in->iat_valid != 0u) && (in->iat_c > rc_cal_get_safety_iat_max_c(cal))) {
        RC_TRIGGER(RC_SAFETY_IAT, "intake air temperature");
        out->action = worst(out->action, RC_ACT_RETARD);
        out->ignition_max_deg = rc_minf(out->ignition_max_deg, out->ignition_max_deg - 5.0f);
    }
    if ((in->oil_temp_valid != 0u) && (in->oil_temp_c > rc_cal_get_safety_oil_temp_max_c(cal))) {
        RC_TRIGGER(RC_SAFETY_OIL_TEMP, "oil temperature");
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.5f);
    }
    if ((in->egt_valid != 0u) && (in->egt_c > rc_cal_get_safety_egt_max_c(cal))) {
        RC_TRIGGER(RC_SAFETY_EGT, "exhaust gas temperature");
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.6f);
    }

    /* ------------------------------------------------------------- oil press */
    if (in->oil_pressure_valid != 0u) {
        if (in->oil_pressure_bar < rc_cal_get_safety_oil_pressure_min_bar(cal)) {
            if (state->oil_since_us == 0u) {
                state->oil_since_us = now_us;
            }
            if (rc_ticks_forward(now_us, state->oil_since_us) >= state->oil_hold_us) {
                state->oil_latched = true;
            }
        } else {
            state->oil_since_us = 0u;
        }
        if (state->oil_latched != false) {
            RC_TRIGGER(RC_SAFETY_OIL_PRESSURE, "oil pressure");
            out->action = worst(out->action, RC_ACT_SHUTDOWN);
            out->fuel_cut = true;
            out->spark_cut = true;
            out->torque_factor = 0.0f;
        }
    }

    /* ------------------------------------------------------------- pressure */
    if ((in->fuel_pressure_valid != 0u)
        && (in->fuel_pressure_kpa < rc_cal_get_safety_fuel_pressure_min_bar(cal) * 100.0f)) {
        RC_TRIGGER(RC_SAFETY_FUEL_PRESSURE, "fuel pressure");
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.5f);
    }

    /* -------------------------------------------------------------- battery */
    if (in->battery_valid != 0u) {
        if (in->battery_v < rc_cal_get_safety_battery_min_v(cal)) {
            RC_TRIGGER(RC_SAFETY_BATTERY, "battery undervoltage");
            /* Low voltage is an enrichment-free enrichment: it cannot be fixed with
             * fuel, and adding fuel raises the load on an already struggling supply. */
            out->action = worst(out->action, RC_ACT_WARNING);
            out->ignition_max_deg = rc_minf(out->ignition_max_deg, out->ignition_max_deg - 8.0f);
        } else if (in->battery_v > rc_cal_get_safety_battery_max_v(cal)) {
            RC_TRIGGER(RC_SAFETY_BATTERY, "battery overvoltage");
            out->action = worst(out->action, RC_ACT_LIMIT);
            out->torque_factor = rc_minf(out->torque_factor, 0.7f);
        }
    }

    /* ------------------------------------------------- actuator headroom */
    if (in->injector_duty_percent >= rc_cal_get_safety_injector_duty_max_pct(cal)) {
        RC_TRIGGER(RC_SAFETY_INJECTOR_DUTY, "injector duty ceiling");
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.6f);
    }
    if (in->dwell_ms >= rc_cal_get_safety_dwell_max(cal)) {
        RC_TRIGGER(RC_SAFETY_DWELL, "dwell ceiling");
        out->action = worst(out->action, RC_ACT_LIMIT);
    }
    if (in->boost_kpa > rc_cal_get_safety_boost_max_kpa(cal)) {
        RC_TRIGGER(RC_SAFETY_BOOST, "overboost");
        out->action = worst(out->action, RC_ACT_CUT);
        out->fuel_cut = true;
        out->torque_factor = 0.0f;
    }

    /* ------------------------------------------------------ sync and sensors */
    if (in->sync_lost != 0u) {
        RC_TRIGGER(RC_SAFETY_SYNC, "crank synchronisation lost");
        out->action = worst(out->action, RC_ACT_CUT);
        out->fuel_cut = true;
        out->spark_cut = true;
        out->torque_factor = 0.0f;
        /* Ignition is pulled down to the floor as well as cut, so a spark that has
         * already been scheduled cannot land at a stale angle. */
        out->ignition_max_deg = out->ignition_min_deg;
    }
    if (in->sensor_fault != 0u || sensor_degraded) {
        RC_TRIGGER(RC_SAFETY_SENSOR, "sensor fault");
        /* A faulted sensor slows the engine rather than stopping it, unless the
         * declared fault behaviour for that channel is a cut.  Degradation beats an
         * abrupt stop for a rider mid-corner. */
        out->action = worst(out->action, RC_ACT_LIMIT);
        out->torque_factor = rc_minf(out->torque_factor, 0.5f);
    }

#undef RC_TRIGGER

    if (out->fuel_cut != false || out->spark_cut != false) {
        out->torque_factor = 0.0f;
    }
    state->last_action = out->active;
    state->last_reset_us = now_us;
}

/** Release hysteresis, as a fraction of the trip threshold. */
#define RC_RELEASE_HYSTERESIS 0.9f

void rc_safety_service_latches(rc_safety_state_t *state, const rc_cal_t *cal,
                               const rc_safety_input_t *in, uint32_t now_us)
{
    if (state == NULL || cal == NULL || in == NULL) {
        return;
    }
    /* Release predicates, each with hysteresis so a value hovering at the limit does not
     * chatter.  Only the safety module releases its own latches. */
    if (state->lean_latched != false && in->lambda_valid != 0u
        && in->lambda <= rc_cal_get_safety_lean_lambda_limit(cal) * RC_RELEASE_HYSTERESIS) {
        state->lean_latched = false;
        state->lean_since_us = 0u;
    }
    if (state->oil_latched != false && in->oil_pressure_valid != 0u
        && in->oil_pressure_bar >= rc_cal_get_safety_oil_pressure_recovery_bar(cal)) {
        state->oil_latched = false;
        state->oil_since_us = 0u;
    }
    (void)now_us;
}

const char *rc_safety_reason(uint32_t active)
{
    if ((active & RC_SAFETY_SYNC) != 0u) {
        return "crank synchronisation lost: the decoder is not SYNCED or DEGRADED, so the "
               "ECU cannot know where any cylinder is. Consequence: fuel and spark are cut "
               "and ignition is pulled to the floor. Test: check CKP/CMP conditioning, "
               "wiring, the 5 V reference and the trigger configuration.";
    }
    if ((active & RC_SAFETY_OVERSPEED) != 0u) {
        return "overspeed: measured speed exceeded the instant fault threshold. Consequence: "
               "immediate shutdown. Test: check for a mechanical overspeed, a wrong trigger "
               "configuration, or a stuck throttle.";
    }
    if ((active & RC_SAFETY_RPM) != 0u) {
        return "rev limit: measured speed reached the limiter. Consequence: fuel and spark "
               "cut until speed falls below the limit plus hysteresis. This is normal "
               "operation at the limiter, not a fault.";
    }
    if ((active & RC_SAFETY_LEAN) != 0u) {
        return "lean limit: measured lambda stayed leaner than the limit for the hold time. "
               "Consequence: fuel cut. Test: check injectors, fuel pressure, lambda sensor "
               "position, and whether a hole has been pulled in the exhaust.";
    }
    if ((active & RC_SAFETY_OIL_PRESSURE) != 0u) {
        return "oil pressure: measured pressure stayed below the minimum for the hold time. "
               "Consequence: shutdown. Test: stop the engine and check oil level and oil "
               "pump before anything else.";
    }
    if ((active & RC_SAFETY_ECT) != 0u) {
        return "engine temperature: above the limit. Consequence: torque limited. Test: "
               "check the coolant circuit, the fan output and the sensor.";
    }
    if ((active & RC_SAFETY_EGT) != 0u) {
        return "exhaust gas temperature: above the limit. Consequence: torque limited. "
               "Note this limit is a placeholder pending probe placement and dyno work "
               "(UNK-GT-018, SAFETY_CASE section 6).";
    }
    if ((active & RC_SAFETY_INJECTOR_DUTY) != 0u) {
        return "injector duty: the commanded duty reached the ceiling. Consequence: torque "
               "limited. Test: check for a leaking fuel pressure regulator or an "
               "insufficient pressure differential.";
    }
    if ((active & RC_SAFETY_FUEL_PRESSURE) != 0u) {
        return "fuel pressure: below the minimum. Consequence: torque limited. Test: check "
               "the pump, the filter and the regulator.";
    }
    if ((active & RC_SAFETY_BATTERY) != 0u) {
        return "battery voltage outside the working range. Consequence: advance is cut on "
               "undervoltage, torque limited on overvoltage. Test: check the charging "
               "system and the sensor wiring.";
    }
    if ((active & RC_SAFETY_SENSOR) != 0u) {
        return "sensor fault: at least one channel is not usable. Consequence: the affected "
               "function degrades to its declared fallback and torque is limited. Test: the "
               "ECU health page names the channel and the fault.";
    }
    if ((active & RC_SAFETY_BOOST) != 0u) {
        return "overboost: measured boost above the hard limit. Consequence: fuel cut. "
               "Test: check the wastegate and the boost control loop.";
    }
    if (active == 0u) {
        return "no active safety limit";
    }
    return "one or more limits active; see the ECU health page for the full set";
}
