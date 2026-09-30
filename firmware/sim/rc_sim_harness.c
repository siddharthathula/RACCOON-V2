/* RACCOON V2 - software-in-the-loop harness.  See rc_sim_harness.h. */
#include "rc/engine.h"
#include "rc_sim_harness.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "cal_types.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/safety.h"

/* The plant's own copy of the fuel hardware, which the ECU is NOT told about.  A wrong
 * value here is a miscalibrated injector, which is the most common real fault there is,
 * and the closed loop should correct for it rather than the engine dying. */
#define SIM_INJECTOR_FLOW_CC_PER_S 10.0f
#define SIM_FUEL_DENSITY 745.0f
#define SIM_FUEL_PRESSURE_KPA 380.0f
#define SIM_INJECTOR_DEAD_TIME_MS 0.9f
#define SIM_ADC_FULL_SCALE 4095u

/* --- sensor transfer functions.  The ECU is told the slope; the plant applies it. ------- */

/* Dither, in ADC counts.
 *
 * A perfectly constant raw reading is not something a real converter produces: the input
 * is analog, the reference is noisy, and the result always moves by a count or two.  A
 * plant that emits identical counts forever makes every frozen-detection path fire on a
 * perfectly healthy sensor, which either hides the real bug or sends the whole test suite
 * looking for one.  One count of dither is the smallest honest amount. */
static uint16_t adc_dither(uint16_t counts, uint32_t *state)
{
    *state = (*state * 1103515245u) + 12345u;
    const int step = (int)((*state >> 16) & 1u) - (int)((*state >> 17) & 1u);
    const int value = (int)counts + step;
    if (value < 0) {
        return 0u;
    }
    if (value > (int)SIM_ADC_FULL_SCALE) {
        return (uint16_t)SIM_ADC_FULL_SCALE;
    }
    return (uint16_t)value;
}

/* A sensor is a physical part: it puts out a VOLTAGE proportional to whatever it measures,
 * and the converter digitises that voltage.  The ECU's calibration describes each sensor in
 * engineering units per volt, so the plant has to work in volts too.
 *
 * Encoding the plant's value with an independent hard-coded slope instead is the classic
 * way to get a simulator that agrees with nothing: the ECU sees a pressure that differs
 * from the truth by whatever ratio the two slopes happen to have, the reading falls outside
 * its own declared range, and the channel is faulted for the entire run.  The test then
 * appears to be about the ECU while every failure is really about the fixture. */
static uint16_t adc_from_volts(float volts)
{
    if (volts < 0.0f) {
        volts = 0.0f;
    }
    if (volts > 5.0f) {
        volts = 5.0f;
    }
    return (uint16_t)((volts * (SIM_ADC_FULL_SCALE / 5.0f)) + 0.5f);
}

static uint16_t adc_encode(float engineering, float slope, float offset, float min, float max)
{
    if (!(slope != 0.0f)) {
        return 0u;
    }
    float counts = ((engineering - offset) / slope) + 0.5f;
    /* Clamp to the rails rather than wrapping: an out-of-range sensor pins high or low in
     * a real circuit, and the open/short detector in the ECU depends on that. */
    if (counts < 0.0f) {
        counts = 0.0f;
    }
    if (counts > (float)SIM_ADC_FULL_SCALE) {
        counts = (float)SIM_ADC_FULL_SCALE;
    }
    (void)min;
    (void)max;
    return (uint16_t)counts;
}

void rc_harness_default_tune(uint8_t cylinders, uint8_t ckp_teeth, uint8_t ckp_missing,
                             uint8_t cmp_teeth, float swept_cc, rc_cal_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_cylinder_count(out, cylinders);
    (void)rc_cal_set_engine_crank_layout(out, 0u);
    (void)rc_cal_set_trigger_teeth(out, ckp_teeth);
    (void)rc_cal_set_trigger_missing_teeth(out, ckp_missing);
    /* The profile is a SELECTOR and the tooth count is a separate item, and they are not
     * interchangeable. Passing a tooth count as the profile worked only because every
     * non-zero profile used to mean the same 3 teeth; now that a 4-tooth wheel is decoded as
     * 4 teeth, "4" selects the CUSTOM profile, which has to be given an actual tooth count -
     * and so a harness built for a 4-tooth cam silently stopped configuring.
     *
     * Which is the right outcome: a custom wheel with no tooth count is not a wheel. */
    if (cmp_teeth == 3u) {
        (void)rc_cal_set_trigger_profile_cam(out, 1u);   /* mt_3_1 */
    } else if (cmp_teeth == 4u) {
        (void)rc_cal_set_trigger_profile_cam(out, 2u);   /* mt_4_1 */
    } else if (cmp_teeth == 1u) {
        (void)rc_cal_set_trigger_profile_cam(out, 3u);   /* one edge per cam revolution */
    } else if (cmp_teeth >= 2u) {
        (void)rc_cal_set_trigger_profile_cam(out, 4u);   /* custom */
        (void)rc_cal_set_trigger_cam_teeth(out, (uint8_t)cmp_teeth);
        (void)rc_cal_set_trigger_cam_missing_teeth(out, 1u);
    } else {
        (void)rc_cal_set_trigger_profile_cam(out, 0u);   /* none */
    }
    (void)rc_cal_set_trigger_input_filter_level(out, 1u);
    (void)rc_cal_set_trigger_sync_skip_revolutions(out, 0u);
    /* Both conditioner outputs active high. This is part of describing the vehicle: the
     * harness speaks the wiring the calibration declares, and a polarity left at the shipped
     * default (active low) means the plant emits edges the ECU correctly ignores - so the
     * engine never starts and the reason is nowhere near the polarity. */
    (void)rc_cal_set_trigger_trigger_edge_polarity(out, 0x03u);
    (void)rc_cal_set_strategy_tdc_verified(out, true);
    (void)rc_cal_set_strategy_cam_phase_verified(out, true);
    (void)rc_cal_set_strategy_lambda_strategy(out, 1u);
    (void)rc_cal_set_strategy_injection_strategy(out, 1u);
    /* Bore and stroke are DERIVED from the swept volume the harness was asked for.
     *
     * Two things force this. First, rc_control_configure refuses a tune whose stated
     * displacement disagrees with bore x stroke by more than two percent - correctly,
     * because a tune whose own numbers contradict each other is not one to run. Second, the
     * shipped bore and stroke describe roughly a 21 cc engine, which is a placeholder rather
     * than a design, so scaling them to a 250 cc cylinder would need a 536 mm bore and be
     * refused for being outside the item's own range anyway.
     *
     * So: pick a stroke in the middle of what this engine class actually uses, and solve
     * for the bore. Both are then clamped to their declared ranges, and the displacement
     * stated is what the CLAMPED geometry actually produces - not what was asked for. A
     * harness that quietly reported a swept volume it is not simulating would make every
     * fuel calculation downstream wrong in a way nothing else would catch. */
    const float stroke_mm = 70.0f;
    /* Solved from the SAME helper the engine builder uses, so the arithmetic cannot drift
     * from the check it has to satisfy. Displacement is (pi/4) * bore^2 * stroke, not
     * bore * stroke - getting that wrong is a factor of about 47 on a 70 mm stroke, and the
     * resulting refusal reads as a geometry problem rather than a formula one. */
    float bore_mm = sqrtf((4.0f * swept_cc * 1000.0f) / (RC_PI_F * stroke_mm));
    if (bore_mm < 20.0f) {
        bore_mm = 20.0f;
    } else if (bore_mm > 150.0f) {
        bore_mm = 150.0f;
    }
    (void)rc_cal_set_engine_bore_mm(out, bore_mm);
    (void)rc_cal_set_engine_stroke_mm(out, stroke_mm);
    /* The displacement that geometry ACTUALLY is, not the one that was requested. */
    (void)rc_cal_set_engine_displacement_cc(out, rc_engine_displacement_cc(bore_mm, stroke_mm,
                                                                           cylinders));

}

void rc_harness_init(rc_harness_t *h, float swept_cc, uint8_t cylinders, uint8_t ckp_teeth,
                     uint8_t ckp_missing, uint8_t cmp_teeth)
{
    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof(*h));
    h->ckp_teeth = ckp_teeth;
    h->ckp_missing = ckp_missing;
    h->cmp_teeth = cmp_teeth;
    h->step_us = 100.0f; /* 10 kHz: fine enough to resolve a 1 ms tooth at idle */

    rc_sim_init(&h->sim, swept_cc, cylinders, 9.5f);
    rc_hal_virtual_init(&h->hal);
    rc_control_init(&h->control);

    /* The fault struct was zeroed above, and a zero flow scale would mean "this vehicle's
     * injectors deliver nothing at all" - a silently dead plant that looks exactly like a
     * dead ECU.  Every field that is a MULTIPLIER rather than a boolean must therefore be
     * given its neutral value explicitly, or the default run tests nothing. */
    h->faults.injector_flow_scale = 1.0f;
    h->faults.battery_v = 0.0f; /* 0 means "not faulted; use nominal" */

    /* Build a tune that matches the geometry this harness was given, and install it.

     * The injector flow in the calibration is the ECU's belief; SIM_INJECTOR_FLOW_CC_PER_S is
     * the truth, and they start out equal.
     *
     * Without this the harness keeps the shipped defaults, whose trigger profile describes
     * a different engine, and rc_control_arm refuses with "the engine geometry has not been
     * built". Every scenario used to work around that by building its own tune, which meant
     * a harness could not be used on its own - and a link server, which has no scenario to
     * borrow the helper from, comes up with an engine that will not start. */
    rc_cal_t built;
    rc_harness_default_tune(cylinders, ckp_teeth, ckp_missing, cmp_teeth, swept_cc, &built);
    rc_harness_set_cal(h, &built);
}

void rc_harness_set_cal(rc_harness_t *h, const rc_cal_t *cal)
{
    if (h == NULL || cal == NULL) {
        return;
    }
    /* Bound the trigger timing before configuring: a scenario that supplies a period of
     * zero must not be able to divide by it, and a real configure would refuse it anyway. */
    rc_cal_t safe = *cal;
    rc_cal_apply_table_fills(&safe);
    rc_cal_apply_axis_defaults(&safe);
    if (safe.trigger_min_period_us == 0u) {
        safe.trigger_min_period_us = 20u;
    }
    if (safe.trigger_overspeed_period_us == 0u) {
        safe.trigger_overspeed_period_us = 30u;
    }
    if (safe.trigger_max_period_us == 0u) {
        safe.trigger_max_period_us = 2000000u;
    }

    /* Go through the ECU's own configuration path rather than writing the calibration
     * straight into the control structure.  That path rebuilds the engine AND
     * reconfigures the trigger, which is what makes a new tune actually take effect: a
     * harness that pokes the calibration alone leaves the decoder configured for the
     * PREVIOUS tune, so a scenario believes it has changed the wheel and the cam and the
     * injector and nothing downstream ever hears about it.  Every failure then looks like
     * an ECU bug and none of them is one. */
    const bool was_armed = (h->control.armed != 0u);
    const char *reason = NULL;
    const bool ok = rc_control_configure(&h->control, &safe, &reason);
    if (ok == false) {
        /* Deliberately not fatal.  A scenario exists to prove the ECU refuses a
         * configuration it cannot build, and carrying on with an empty geometry is exactly
         * what the real ECU does; the arm gate is what must stop it running. */
        h->control.engine.cylinder_count = 0u;
        h->configure_reason = reason;
    } else {
        h->configure_reason = NULL;
    }
    /* Re-register the inputs for the new tune: MAP's transfer function is calibration. */
    rc_control_configure_inputs(&h->control);
    if (was_armed) {
        rc_harness_set_armed(h, true);
    }
}

void rc_harness_set_armed(rc_harness_t *h, bool armed)
{
    if (h == NULL) {
        return;
    }
    const char *reason = NULL;
    (void)rc_control_arm(&h->control, armed, &reason);
    h->armed = h->control.armed != 0u;
}

/* --------------------------------------------------------------- the event loop */

/** The sim's timer compare.  The ECU armed an event for an absolute tick; when simulated
 *  time reaches it, the edge happens and the plant consumes it. */
static void fire_event(rc_harness_t *h, const rc_event_t *event)
{
    const rc_cal_t *cal = &h->control.cal;
    h->events_fired++;
    if (event->type < RC_EV_TYPE_COUNT) {
        h->events_by_type[event->type]++;
    }

    switch (event->type) {
        case RC_EV_INJECTOR_OPEN:
            /* The OPEN edge only starts the pulse; the width comes from the CLOSE edge. */
            h->sim.injector_open_ms[event->cylinder] = 0.0f;
            break;
        case RC_EV_INJECTOR_CLOSE: {
            /* The width the ECU actually asked for is in the published state, so the
             * harness uses that rather than re-deriving it. */
            const float ms = h->control.state.injector_ms[event->cylinder];
            const float flow = rc_cal_get_fuel_injector_flow_cc_per_s(cal)
                * h->faults.injector_flow_scale;
            rc_sim_inject(&h->sim, event->cylinder, ms, SIM_INJECTOR_DEAD_TIME_MS, flow,
                          SIM_FUEL_DENSITY, SIM_FUEL_PRESSURE_KPA);
            break;
        }
        case RC_EV_COIL_CHARGE:
            break;
        case RC_EV_COIL_FIRE: {
            const float angle = h->control.state.ignition_deg[event->cylinder];
            const float dwell = h->control.state.dwell_ms[event->cylinder];
            if (h->control.state.ignition_cut[event->cylinder] != 0u) {
                h->sim.spark_cut[event->cylinder] = true;
                break;
            }
            h->sim.spark_cut[event->cylinder] = false;
            rc_sim_spark(&h->sim, event->cylinder, angle, dwell, h->sim.iat_c,
                         h->sim.map_kpa);
            break;
        }
        case RC_EV_FUEL_CUT:
            h->sim.fuel_cut = true;
            h->saw_fuel_cut = true;
            break;
        case RC_EV_IGNITION_CUT:
            for (uint8_t c = 0u; c < RC_HARNESS_CYLINDERS; ++c) {
                h->sim.spark_cut[c] = true;
            }
            break;
        case RC_EV_VVT:
        case RC_EV_IDLE_ACTUATOR:
        case RC_EV_BOOST:
        case RC_EV_LAUNCH:
        case RC_EV_SHIFT_CUT:
        case RC_EV_TRACTION:
        case RC_EV_AUX:
        default:
            /* Not modelled as plant physics; the ECU still armed them, which is what
             * this harness is checking. */
            break;
    }
}

static int harness_arm(void *user, const rc_event_t *event)
{
    rc_harness_t *h = (rc_harness_t *)user;
    if (h == NULL || event == NULL) {
        return RC_HAL_ERR_PARAM;
    }
    /* One-shot: re-arming the same channel for the same plan generation is what a real
     * compare register does, and hiding it would hide a scheduler bug. */
    for (uint16_t i = 0u; i < h->pending_count; ++i) {
        if (h->pending[i].used && h->pending[i].event.type == event->type
            && h->pending[i].event.cylinder == event->cylinder
            && h->pending[i].event.generation == event->generation) {
            return RC_HAL_ERR_REFUSED;
        }
    }
    if (h->pending_count >= RC_HARNESS_MAX_PENDING) {
        h->events_dropped++;
        return RC_HAL_ERR_BUSY;
    }
    h->pending[h->pending_count].event = *event;
    h->pending[h->pending_count].used = true;
    h->pending_count++;
    return RC_HAL_OK;
}

static void harness_disarm(void *user, uint8_t type, uint8_t cylinder, uint16_t generation)
{
    rc_harness_t *h = (rc_harness_t *)user;
    if (h == NULL) {
        return;
    }
    for (uint16_t i = 0u; i < h->pending_count; ++i) {
        if (h->pending[i].used && h->pending[i].event.type == type
            && h->pending[i].event.cylinder == cylinder
            && h->pending[i].event.generation <= generation) {
            h->pending[i].used = false;
        }
    }
}

static void harness_force_safe(void *user, uint8_t type, uint8_t cylinder)
{
    (void)cylinder;
    rc_harness_t *h = (rc_harness_t *)user;
    if (h == NULL) {
        return;
    }
    if (type == (uint8_t)RC_EV_FUEL_CUT) {
        h->sim.fuel_cut = true;
    }
    if (type == (uint8_t)RC_EV_IGNITION_CUT) {
        for (uint8_t c = 0u; c < RC_HARNESS_CYLINDERS; ++c) {
            h->sim.spark_cut[c] = true;
        }
    }
}

static uint32_t harness_now(void *user)
{
    rc_harness_t *h = (rc_harness_t *)user;
    return (h != NULL) ? h->sim.now_us : 0u;
}

static int harness_bus_send(void *user, uint8_t channel, const rc_bus_frame_t *frame)
{
    (void)user;
    (void)channel;
    (void)frame;
    return RC_HAL_OK;
}

/* ----------------------------------------------------------------- the ADC frame */

static void build_adc_frame(rc_harness_t *h, rc_adc_frame_t *frame)
{
    const rc_sim_t *s = &h->sim;
    const rc_sim_faults_t *f = &h->faults;
    memset(frame, 0, sizeof(*frame));
    frame->tick_us = s->now_us;
    frame->sequence = ++h->adc_sequence;
    frame->count = (uint8_t)RC_MAX_SENSORS;

    /* 12-bit counts, computed with the same slopes the calibration declares, so a
     * mis-declared slope shows up as a misreading rather than being hidden. */
    const rc_cal_t *cal = &h->control.cal;
    static uint32_t dither;

    float tps = h->rider.throttle_pct;
    if (f->tps_stuck_low) {
        tps = 3.0f;
    } else if (f->tps_stuck_high) {
        tps = 97.0f;
    }
    /* A collapsed 5 V reference drags every railed sensor to the bottom of the range, which
     * is what actually happens and what the dependent-invalidation logic is for. */
    const float rail_scale = f->map_rail ? 0.02f : 1.0f;
    const uint16_t open = (f->map_open || f->ect_open) ? 0u : (uint16_t)SIM_ADC_FULL_SCALE;

    frame->raw[RC_IN_TPS1] = adc_dither(adc_encode(tps * rail_scale, 40.0f, 0.0f, 0.0f, 100.0f), &dither);
    frame->raw[RC_IN_TPS2] = frame->raw[RC_IN_TPS1];
    frame->raw[RC_IN_PEDAL1] = adc_dither(adc_encode(tps, 40.0f, 0.0f, 0.0f, 100.0f), &dither);
    frame->raw[RC_IN_PEDAL2] = frame->raw[RC_IN_PEDAL1];

    float map = s->map_kpa;
    if (f->map_open) {
        frame->raw[RC_IN_MAP] = adc_dither(open, &dither);
    } else {
        /* MAP, through the very slope the calibration declares, so the plant and the ECU
         * can never disagree about what the sensor is reading. */
        const float per_volt = rc_cal_get_sensors_map_slope(cal);
        const float volts = (per_volt > 0.0f) ? (map * rail_scale) / per_volt : 0.0f;
        frame->raw[RC_IN_MAP] = adc_dither(adc_from_volts(volts), &dither);
    }
    frame->raw[RC_IN_MAF] = adc_dither(adc_encode(0.0f * rail_scale, 0.0654f, 0.0f, 0.0f, 200.0f), &dither);
    frame->raw[RC_IN_ECT] = f->ect_open
        ? open
        : adc_encode(s->ect_c * rail_scale, 0.0610f, -20.0f, -20.0f, 150.0f);
    frame->raw[RC_IN_IAT] = adc_dither(adc_encode(s->iat_c * rail_scale, 0.0610f, -20.0f, -20.0f, 150.0f), &dither);
    frame->raw[RC_IN_OIL_P] = adc_dither(adc_encode(s->oil_pressure_bar, 0.0041f, 0.0f, 0.0f, 8.0f), &dither);
    frame->raw[RC_IN_OIL_T] = adc_dither(adc_encode(s->oil_temp_c, 0.0610f, -20.0f, -20.0f, 200.0f), &dither);
    frame->raw[RC_IN_FUEL_P] = adc_dither(adc_encode(SIM_FUEL_PRESSURE_KPA, 0.1220f, 0.0f, 0.0f, 500.0f), &dither);
    frame->raw[RC_IN_EGT] = adc_dither(adc_encode(s->egt_c, 0.0610f, 0.0f, 0.0f, 1200.0f), &dither);
    frame->raw[RC_IN_ETHANOL] = adc_dither(adc_encode(
        rc_cal_get_engine_fuel_ethanol_fraction(cal) * 100.0f, 0.0400f, 0.0f, 0.0f, 100.0f), &dither);
    frame->raw[RC_IN_BATTERY] = adc_dither(adc_encode(
        (h->faults.battery_v > 0.0f) ? h->faults.battery_v : 13.8f, 0.0442f, 0.0f, 0.0f, 20.0f), &dither);
    frame->raw[RC_IN_REF5V] = f->map_rail ? 0u : (uint16_t)SIM_ADC_FULL_SCALE;
    const float lambda_seen = s->lambda + f->lambda_bias;
    frame->raw[RC_IN_LAMBDA1] = adc_dither(adc_from_volts(lambda_seen * 0.8f), &dither);
    frame->raw[RC_IN_LAMBDA2] = frame->raw[RC_IN_LAMBDA1];
    frame->raw[RC_IN_LAMBDA3] = frame->raw[RC_IN_LAMBDA1];
    frame->raw[RC_IN_LAMBDA4] = frame->raw[RC_IN_LAMBDA1];
    frame->raw[RC_IN_WHEEL_F] = adc_dither(adc_encode(s->front_wheel_kph, 1.0f, 0.0f, 0.0f, 250.0f), &dither);
    frame->raw[RC_IN_WHEEL_R] = adc_dither(adc_encode(s->rear_wheel_kph, 1.0f, 0.0f, 0.0f, 250.0f), &dither);
}

/* --------------------------------------------------------------------- one step */

static bool sim_finite(float v)
{
    return (v == v) && (v > -1.0e30f) && (v < 1.0e30f);
}

static void check_published(rc_harness_t *h)
{
    const rc_engine_state_t *s = &h->control.state;
    if (!sim_finite(s->load) || !sim_finite((float)s->pos.rpm_x2 * 0.5f) || !sim_finite(s->lambda[0])) {
        h->non_finite_seen = true;
    }
    for (uint8_t c = 0u; c < RC_HARNESS_CYLINDERS; ++c) {
        if (!sim_finite(s->injector_ms[c]) || !sim_finite(s->ignition_deg[c]) || !sim_finite(s->dwell_ms[c])) {
            h->non_finite_seen = true;
        }
    }
    h->last_lambda = s->lambda[0];
    h->last_rpm = (float)s->pos.rpm_x2 * 0.5f;
    h->last_pulse_ms = s->injector_ms[0];
    h->last_ignition_deg = s->ignition_deg[0];
}

void rc_harness_step(rc_harness_t *h, float dt_s)
{
    if (h == NULL || dt_s <= 0.0f) {
        return;
    }
    h->dt_s = dt_s;
    h->step_us = dt_s * 1.0e6f;
    h->sim_steps++;

    /* 1. Anything the ECU armed earlier comes due.  This happens BEFORE the control cycle,
     *    because on real hardware a compare interrupt that is already pending is serviced
     *    first: the edge happened at its target tick, not at the next cycle.
     *
     *    The list is COMPACTED as it goes.  A fired event is dead weight, and a list that
     *    only ever grows fills up within a few cycles and then silently refuses every new
     *    event - which looks exactly like a dead ECU, because the engine simply stops
     *    getting fuel and nothing reports an error. */
    {
        uint16_t i = 0u;
        uint16_t kept = 0u;
        while (i < h->pending_count) {
            if (h->pending[i].used) {
                if (rc_deadline_ok(h->sim.now_us, h->pending[i].event.target_tick) == false) {
                    fire_event(h, &h->pending[i].event);
                } else {
                    h->pending[kept++] = h->pending[i];
                }
            }
            ++i;
        }
        h->pending_count = kept;
    }

    /* 2. Crank and cam edges, generated from the engine's own angle.  A stopped engine has
     *    no trigger, which is why a cranking engine is a distinct and testable state. */
    {
        /* A conditioner configured active-low emits a LOW-going edge, and the ECU is
         * entitled to ignore the other one: feeding level 1 regardless of the configured
         * polarity silently produces no trigger at all, and every scenario then looks like
         * an ECU that cannot decode anything.  The plant has to speak the wiring the
         * calibration describes. */
        const uint8_t crank_level = (uint8_t)((h->control.cal.trigger_trigger_edge_polarity & 0x01u)
                                              ? 1u : 0u);
        const uint8_t cam_level = (uint8_t)((h->control.cal.trigger_trigger_edge_polarity & 0x02u)
                                            ? 1u : 0u);
        const float dt_ckp = rc_sim_time_to_next_ckp(&h->sim, h->ckp_teeth, h->ckp_missing);
        if (!h->suppress_trigger && dt_ckp > 0.0f && dt_ckp <= dt_s && h->sim.rpm > 1.0f) {
            rc_control_on_capture(&h->control, 0u, h->sim.now_us, crank_level);
        }
        /* A cam edge is produced when the crank ANGLE crosses a cam tooth, not when a
         * time window happens to contain the next edge.  The two are equivalent in
         * continuous time, but on a discrete step the window test only fires if a step
         * lands inside it - which at 10 kHz and a half-revolution cam interval is a few
         * percent of steps, so the cam appears to work intermittently and the sequential
         * gate closes and opens for no reason that is visible anywhere. */
        if (h->cmp_teeth > 0u && h->sim.rpm > 1.0f) {
            const float span = 360.0f / (float)h->cmp_teeth;
            const float before = h->sim.last_crank_deg;
            const float after = h->sim.crank_angle_deg;
            const uint32_t first = (uint32_t)floorf(before / span) + 1u;
            float crossed = (float)first * span;
            if (crossed < after) {
                rc_control_on_capture(&h->control, 1u, h->sim.now_us, cam_level);
            }
        }
    }

    /* 3. The sensor frame, and the plant.  Plant first, so the sensors report the state
     *    the engine has actually reached, not the one it was commanded into. */
    rc_adc_frame_t frame;
    build_adc_frame(h, &frame);
    rc_control_on_adc(&h->control, &frame);

    /* 4. The control cycle, then the services that push events to the hardware. */
    rc_control_cycle(&h->control, h->sim.now_us);
    h->hal.base.arm_event = harness_arm;
    h->hal.base.disarm_event = harness_disarm;
    h->hal.base.force_safe_output = harness_force_safe;
    h->hal.base.tick_now_us = harness_now;
    h->hal.base.bus_send = harness_bus_send;
    h->hal.base.owner = h;
    rc_control_service(&h->control, h->sim.now_us, &h->hal.base, h);
    h->cycles_run++;
    check_published(h);

    if (h->control.state.safety_active != 0u) {
        if ((h->control.state.action & (uint8_t)RC_ACT_CUT) != 0u) {
            h->saw_fuel_cut = true;
        }
    }
    if (h->control.trigger.state != RC_TRIGGER_SYNCED) {
        h->saw_sync_loss = true;
    }

    /* The idle valve is an output like any other: the ECU publishes it, the plant opens
     * its bypass by that much. */
    h->sim.idle_valve_pct = h->control.state.idle_valve_pct;

    /* 5. The plant advances.  Fuel and torque from the edges that already fired, plus
     *    whatever the crank did. */
    rc_sim_step(&h->sim, &h->rider, &h->faults, dt_s);
}

void rc_harness_run(rc_harness_t *h, float seconds, float dt_s)
{
    if (h == NULL || dt_s <= 0.0f || seconds <= 0.0f) {
        return;
    }
    /* A hard cap on steps: a scenario that hangs must fail the test rather than hang the
     * suite, so "run for 10 s" can never become "run forever". */
    const uint32_t max_steps = (uint32_t)(seconds / dt_s) + 4u;
    for (uint32_t i = 0u; i < max_steps; ++i) {
        rc_harness_step(h, dt_s);
    }
}
