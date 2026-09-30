/* RACCOON V2 - the control cycle. See engine_control.h. */
#include "rc/engine_control.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/rc_math.h"


/* ------------------------------------------------------------- input channels */

/* The input table.  Slopes are engineering units per ADC count, so a 0-5 V sensor on a
 * 12-bit converter covers `max / slope` counts.
 *
 * This table is the wiring: it says how each physical input is scaled and what counts as
 * plausible for it.  It is code rather than calibration because a slope is a property of
 * the CIRCUIT, not of a tune - a tuner who could change it would be able to make a broken
 * sensor read plausibly, and the fault detection would silently follow the lie.  Only the
 * handful of items that genuinely are per-engine (MAP, whose sensor and therefore slope
 * varies with the vehicle) come from the calibration, and those are applied below. */
typedef struct {
    uint8_t channel;      /**< rc_input_channel_t */
    float slope;          /**< engineering units per ADC count */
    float offset;
    float min;
    float max;
    float max_rate;       /**< engineering units per second */
    uint32_t freshness_us;
} rc_input_desc_t;

static const rc_input_desc_t k_inputs[] = {
    {RC_IN_TPS1,   40.0f,    0.0f,   0.0f, 100.0f,  5000.0f, 50000u},
    {RC_IN_TPS2,   40.0f,    0.0f,   0.0f, 100.0f,  5000.0f, 50000u},
    {RC_IN_PEDAL1, 40.0f,    0.0f,   0.0f, 100.0f,  5000.0f, 50000u},
    {RC_IN_PEDAL2, 40.0f,    0.0f,   0.0f, 100.0f,  5000.0f, 50000u},
    /* MAP's slope is overridden from the calibration; the rest of the row is a fallback. */
    {RC_IN_MAP,    0.0733f,  0.0f,   0.0f, 300.0f, 20000.0f, 50000u},
    {RC_IN_MAF,    0.0654f,  0.0f,   0.0f, 200.0f,  2000.0f, 50000u},
    {RC_IN_ECT,    0.0610f, -20.0f, -20.0f, 150.0f,   200.0f, 200000u},
    {RC_IN_IAT,    0.0610f, -20.0f, -20.0f, 150.0f,   200.0f, 200000u},
    {RC_IN_OIL_P,  0.0041f,  0.0f,   0.0f,   8.0f,    50.0f, 100000u},
    {RC_IN_OIL_T,  0.0610f, -20.0f, -20.0f, 200.0f,    50.0f, 500000u},
    {RC_IN_FUEL_P, 0.1220f,  0.0f,   0.0f, 500.0f,  1000.0f, 100000u},
    {RC_IN_EGT,    0.0610f,  0.0f,   0.0f,1200.0f,  2000.0f, 200000u},
    {RC_IN_ETHANOL,0.0400f,  0.0f,   0.0f, 100.0f,   100.0f, 500000u},
    {RC_IN_BATTERY,0.0442f,  0.0f,   0.0f,  20.0f,   100.0f, 50000u},
    {RC_IN_REF5V,  0.0012f,  0.0f,   0.0f,   6.0f,    50.0f, 50000u},
    /* A wideband puts out roughly 0.8 V per unit lambda on a 5 V reference, so one lambda
     * sits around 655 counts.  A slope anywhere near "0.5 per count" would put a perfectly
     * healthy reading at four counts - on the bottom rail - where the open-circuit detector
     * correctly reports it as a broken sensor, and the closed loop is then permanently off
     * because the ECU refuses to trust it.  The failure looks like a dead wideband, not
     * like an arithmetic slip. */
    {RC_IN_LAMBDA1,0.0015267f, 0.0f,  0.4f,   2.0f,    20.0f, 50000u},
    {RC_IN_LAMBDA2,0.0015267f, 0.0f,  0.4f,   2.0f,    20.0f, 50000u},
    {RC_IN_LAMBDA3,0.0015267f, 0.0f,  0.4f,   2.0f,    20.0f, 50000u},
    {RC_IN_LAMBDA4,0.0015267f, 0.0f,  0.4f,   2.0f,    20.0f, 50000u},
    {RC_IN_WHEEL_F, 1.0000f,  0.0f,   0.0f, 250.0f,  100.0f, 200000u},
    {RC_IN_WHEEL_R, 1.0000f,  0.0f,   0.0f, 250.0f,  100.0f, 200000u},
};

/** Register every input channel with the sensor layer.
 *
 *  Without this the sensor table is empty, `rc_sensors_update` rejects every frame as an
 *  open circuit, and the control loop runs on nothing: it computes perfect fuel commands
 *  from a load of zero and never notices, because "no sensor" and "no load" look the same
 *  from the outside.  This is the difference between a control system and a collection of
 *  algorithms. */
static uint32_t frozen_window_us(const rc_cal_t *cal)
{
    const uint32_t ms = rc_cal_get_sensors_sensor_frozen_window_ms(cal);
    return (ms > 0u) ? (ms * 1000u) : 2000000u;
}

void rc_control_configure_inputs(rc_control_t *control)
{
    const rc_cal_t *cal = &control->cal;
    rc_sensors_init(&control->sensors);
    for (size_t i = 0u; i < (sizeof(k_inputs) / sizeof(k_inputs[0])); ++i) {
        rc_sensor_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.slope = k_inputs[i].slope;
        cfg.offset = k_inputs[i].offset;
        cfg.min = k_inputs[i].min;
        cfg.max = k_inputs[i].max;
        cfg.max_rate = k_inputs[i].max_rate;
        cfg.freshness_us = k_inputs[i].freshness_us;
        cfg.frozen_window_us = frozen_window_us(cal);
        cfg.behaviour = (uint8_t)RC_SENSOR_FB_SUBSTITUTE;
        cfg.enabled = 1u;
        /* MAP is fitted per vehicle, so its transfer function IS calibration. */
        if (k_inputs[i].channel == (uint8_t)RC_IN_MAP) {
            /* map_slope is specified in kPa per VOLT, which is how a pressure sensor is
             * described on a datasheet and what a tuner will set.  The sensor layer wants
             * engineering units per ADC COUNT.  Converting here - once, in one place - is
             * the difference between a calibration that works and one that is silently off
             * by a factor of 4095, which puts MAP permanently out of range and makes the
             * engine run with no load at all while every other value looks fine.
             *
             *   kPa = (counts / full_scale) * V_ref * slope_per_volt
             *   so kPa per count = V_ref * slope_per_volt / full_scale. */
            const float slope_per_volt = rc_cal_get_sensors_map_slope(cal);
            const float offset = rc_cal_get_sensors_map_offset(cal);
            const float lo = rc_cal_get_sensors_map_min(cal);
            const float hi = rc_cal_get_sensors_map_max(cal);
            if (slope_per_volt > 0.0f) {
                cfg.slope = (slope_per_volt * RC_ADC_REFERENCE_V) / (float)RC_ADC_FULL_SCALE;
            }
            cfg.offset = offset;
            cfg.min = lo;
            cfg.max = hi;
            const float rate = rc_cal_get_sensors_map_rate_limit(cal);
            if (rate > 0.0f) {
                cfg.max_rate = rate;
            }
            const uint32_t fresh_ms = rc_cal_get_sensors_map_freshness_ms(cal);
            if (fresh_ms > 0u) {
                cfg.freshness_us = fresh_ms * 1000u;
            }
        }
        (void)rc_sensors_add(&control->sensors, &cfg);
    }
}

void rc_control_init(rc_control_t *control)
{
    if (control == NULL) {
        return;
    }
    memset(control, 0, sizeof(*control));
    control->cal = rc_cal_defaults;
    rc_cal_apply_table_fills(&control->cal);
    rc_cal_apply_axis_defaults(&control->cal);
    (void)rc_engine_build(&control->cal, &control->engine, NULL);
    (void)rc_trigger_configure(&control->trigger, &control->cal, NULL);
    rc_sched_init(&control->sched);
    rc_control_configure_inputs(control);
    rc_fuel_init(&control->fuel);
    rc_ignition_init(&control->knock);
    rc_torque_init(&control->torque);
    rc_safety_init(&control->safety);
    rc_idle_init(&control->idle);
    rc_perf_init(&control->perf);
    control->init_done = 1u;
    control->armed = 0u;
    control->arm_block_reason = "not configured";
}


bool rc_control_configure(rc_control_t *control, const rc_cal_t *cal, const char **reason)
{
    if (control == NULL || cal == NULL) {
        if (reason != NULL) {
            *reason = "null argument";
        }
        return false;
    }
    /* The ECU is the authority: the same validator the tuner runs is re-run here, so a
     * configuration that is wrong on the wire is refused here regardless of what the GUI
     * believed (CALIBRATION_SPEC.md section 6). */
    uint32_t blocking = 0u;
    if (rc_cal_validate(cal, NULL, 0u, &blocking) == false) {
        control->armed = 0u;
        if (reason != NULL) {
            *reason = "configuration failed validation on the ECU; see the diagnostics view "
                      "for the specific rules";
        }
        return false;
    }

    const char *why = NULL;
    if (rc_engine_build(cal, &control->engine, &why) == false) {
        control->armed = 0u;
        if (reason != NULL) {
            *reason = why;
        }
        return false;
    }
    if (rc_engine_strategy_valid(cal, &why) == false) {
        control->armed = 0u;
        if (reason != NULL) {
            *reason = why;
        }
        return false;
    }
    if (rc_trigger_configure(&control->trigger, cal, &why) == false) {
        control->armed = 0u;
        if (reason != NULL) {
            *reason = why;
        }
        return false;
    }

    control->cal = *cal;
    /* The input table is part of the configuration: MAP's transfer function is
     * calibration, so a new tune means new channels. */
    rc_control_configure_inputs(control);
    control->armed = 0u;
    if (reason != NULL) {
        *reason = "configuration accepted; engine control is not armed";
    }
    return true;
}

/** May the control path drive actuators right now?
 *
 *  Crank sync is necessary but not sufficient.  A sequential or staged strategy fires each
 *  cylinder on its own intake stroke, so it is meaningless - and dangerous - without a cam
 *  phase reference.  This is deliberately NOT part of the arming gate: arming happens on
 *  key-on, before the engine has turned a single revolution, and a gate that required a
 *  synced cam would therefore be unsatisfiable - the engine could never be started to
 *  acquire the cam it was waiting for.  Here, in the cycle, is where it belongs: the
 *  engine is armed, and this decides whether it is allowed to act. */
static bool control_phase_permitted(const rc_control_t *control)
{
    if (rc_trigger_permits_control(&control->trigger) == false) {
        return false;
    }
    const uint8_t inj = control->cal.strategy_injection_strategy;
    const bool needs_cam = (inj == (uint8_t)RC_INJ_SEQUENTIAL
                            || inj == (uint8_t)RC_INJ_STAGED);
    if (needs_cam && control->trigger.cam_teeth > 0u
        && control->trigger.cam_edge_count == 0u) {
        return false;
    }
    return true;
}

bool rc_control_arm(rc_control_t *control, bool arm, const char **reason)
{
    if (control == NULL) {
        return false;
    }
    if (!arm) {
        control->armed = 0u;
        control->arm_block_reason = "disarmed by request";
        if (reason != NULL) {
            *reason = control->arm_block_reason;
        }
        return true;
    }

    /* The arming gate (spec section 98). Every condition is checked and the FIRST failure
     * is reported by name, because "ENGINE CONTROL NOT ARMED" without a reason is useless
     * to a tuner at 2 AM. */
    const char *why = NULL;
    if (control->init_done == 0u) {
        why = "the control system is not initialised";
    } else if (control->engine.cylinder_count == 0u) {
        why = "the engine geometry has not been built";
    } else if (control->cal.strategy_tdc_verified == 0u) {
        why = "TDC has not been verified: confirm it with the timing light or the TDC tool";
    } else if (rc_engine_strategy_valid(&control->cal, &why) == false) {
        /* why set by the strategy check */
    } else if (control->trigger.configured == 0u) {
        /* Only configure a decoder that is not already running.  Re-configuring here
         * would reset the trigger state machine and throw away the sync the engine has
         * already achieved, so the very act of arming would cost the engine its phase. */
        if (rc_trigger_configure(&control->trigger, &control->cal, &why) == false) {
            /* why set by the trigger check */
        }
    }
    if (why == NULL) {
        if (rc_cal_validate(&control->cal, NULL, 0u, NULL) == false) {
            why = "the active configuration does not validate";
        }
    }


    if (why != NULL) {
        control->armed = 0u;
        control->arm_block_reason = why;
        if (reason != NULL) {
            *reason = why;
        }
        return false;
    }

    control->armed = 1u;
    control->arm_block_reason = NULL;
    if (reason != NULL) {
        *reason = "engine control armed";
    }
    return true;
}

void rc_control_on_capture(rc_control_t *control, uint8_t channel, uint32_t tick_us, uint8_t level)
{
    if (control == NULL) {
        return;
    }
    rc_trigger_edge(&control->trigger, channel, tick_us, level);
}

void rc_control_on_adc(rc_control_t *control, const rc_adc_frame_t *frame)
{
    if (control == NULL || frame == NULL) {
        return;
    }
    uint8_t count = frame->count;
    if (count > RC_IN_CHANNEL_COUNT) {
        count = RC_IN_CHANNEL_COUNT;
    }
    for (uint8_t i = 0u; i < count; ++i) {
        rc_sensors_update(&control->sensors, i, frame->raw[i], frame->tick_us, frame->sequence);
    }
}

uint32_t rc_control_engine_time(const rc_control_t *control)
{
    return (control != NULL) ? control->state.engine_time_us : 0u;
}

static float sensor_or(rc_control_t *control, uint8_t index, float fallback)
{
    if (index >= rc_sensors_count(&control->sensors)) {
        return fallback;
    }
    const rc_sensor_t *sensor = &control->sensors.channel[index];
    return rc_sensors_usable(sensor) ? sensor->value.value : fallback;
}

static bool sensor_ok(rc_control_t *control, uint8_t index)
{
    return (index < rc_sensors_count(&control->sensors))
        && rc_sensors_usable(&control->sensors.channel[index]);
}


/* ------------------------------------------------------------------ event plan */

/** Schedule the injector and coil edges for every cylinder, for the coming cycle.
 *
 *  This is the link between "what the engine should be doing" and "what the timer
 *  channels are told to do".  Without it the whole control path computes perfect fuel and
 *  ignition commands that never reach an actuator, and the engine simply does not run.
 *
 *  A four-stroke cycle is 720 degrees.  Each cylinder therefore gets:
 *    - an injector opening at its intake stroke, held for exactly the commanded pulse;
 *    - a coil charge starting ahead of the spark, and a fire at the commanded angle.
 *
 *  Every event is planned a full CYCLE ahead rather than at the last moment, because a
 *  timer compare has to be programmed before the edge it will produce: at 12000 rpm a
 *  cylinder event arrives every 1.25 ms, and finding, computing and arming it inside that
 *  window is not a margin, it is a coin toss. */
static void plan_cycle_events(rc_control_t *control, uint32_t now_us, const rc_engine_t *engine,
                              const rc_fuel_output_t *fuel_out, const rc_ignition_output_t *ign_out,
                              const rc_safety_output_t *safe_out)
{
    const rc_cal_t *cal = &control->cal;
    const rc_trigger_t *trig = &control->trigger;

    if (engine->cylinder_count == 0u) {
        return;
    }
    /* Only a synchronised decoder may drive actuators.  Planning from an unsynchronised
     * position is how an engine gets fuel injected at a crank angle nobody chose. */
    if (control_phase_permitted(control) == false) {
        return;
    }

    /* Time for one 720 degree cycle at the current speed.  A stopped or very slow engine
     * has no usable period, and the plan is simply not advanced - the next cycle will
     * plan it once the crank is turning. */
    const uint32_t tooth_period = (trig->tooth_period_us != 0u) ? trig->tooth_period_us : 0u;
    if (tooth_period == 0u || trig->teeth == 0u) {
        return;
    }
    const uint32_t cycle_period_us = tooth_period * (uint32_t)trig->teeth * 2u;
    if (cycle_period_us == 0u) {
        return;
    }

    /* Plan ONCE per engine cycle, not once per control tick.
     *
     * The control loop runs at kHz, but a 720 degree cycle lasts tens of milliseconds, so
     * re-planning on every tick would generate tens of thousands of plans per cycle, each
     * with a new generation and its own set of events.  The hardware queue fills within
     * milliseconds and then refuses everything new - and it refuses it SILENTLY, so the
     * engine quietly stops receiving fuel while every counter still reports success.
     *
     * The trigger's cycle angle is the clock for this: it wraps to zero once per engine
     * cycle, which is exactly when a fresh plan is due. */
    if (control->planned_cycle == control->cycle_count) {
        return; /* the plan for this cycle is already armed */
    }
    control->planned_cycle = control->cycle_count;
    rc_sched_new_plan(&control->sched);
    const uint16_t generation = rc_sched_generation(&control->sched);

    const float dead_time_ms = rc_cal_get_fuel_injector_dead_time_ms(cal);
    const float min_pulse = rc_cal_get_fuel_injector_min_pulse_ms(cal);

    for (uint8_t cyl = 0u; cyl < engine->cylinder_count && cyl < RC_MAX_CYLINDERS; ++cyl) {
        /* ---- injector ------------------------------------------------- */
        const bool cut = (fuel_out->fuel_cut != false);
        const float pulse = cut ? 0.0f : fuel_out->base_pulse_ms;
        const float spark_angle = ign_out->final_deg[cyl];

        /* The intake event for this cylinder sits a full cycle from the last one, so the
         * plan stays ahead of the engine at all times. */
        const float intake_angle_deg = (float)(cyl * 360u);

        if (pulse > 0.0f && pulse >= min_pulse) {
            const uint32_t open_tick = now_us + (uint32_t)(((float)cycle_period_us
                                                            * (720.0f - intake_angle_deg))
                                                           / 720.0f);
            const uint32_t close_tick = open_tick + (uint32_t)(pulse * 1000.0f);
            (void)rc_sched_schedule(&control->sched, RC_EV_INJECTOR_OPEN, cyl, open_tick,
                                    close_tick + 2000u, 0u);
            (void)rc_sched_schedule(&control->sched, RC_EV_INJECTOR_CLOSE, cyl, close_tick,
                                    close_tick + 2000u, 0u);
        } else {
            /* No fuel for this cylinder: the injector must be commanded OFF explicitly.
             * Leaving the last event standing would keep a stale pulse armed. */
            (void)rc_sched_schedule(&control->sched, RC_EV_FUEL_CUT, cyl,
                                    now_us + (uint32_t)cycle_period_us,
                                    now_us + (uint32_t)cycle_period_us + 2000u, 0u);
        }

        /* ---- coil -------------------------------------------------------- */
        const bool spark_cut = (ign_out->cut[cyl] != false) || (safe_out->spark_cut != false);
        if (spark_cut) {
            (void)rc_sched_schedule(&control->sched, RC_EV_IGNITION_CUT, cyl,
                                    now_us + (uint32_t)cycle_period_us,
                                    now_us + (uint32_t)cycle_period_us + 2000u, 0u);
            continue;
        }
        const float dwell_ms = ign_out->dwell_ms[cyl];
        const float charge_deg = spark_angle - ((dwell_ms * 1000.0f * 360.0f)
                                                / (float)cycle_period_us);
        const uint32_t fire_tick = now_us + (uint32_t)(((float)cycle_period_us
                                                         * (720.0f - spark_angle))
                                                        / 720.0f);
        const uint32_t charge_tick = now_us + (uint32_t)(((float)cycle_period_us
                                                          * (720.0f - charge_deg))
                                                         / 720.0f);
        (void)rc_sched_schedule(&control->sched, RC_EV_COIL_CHARGE, cyl, charge_tick, fire_tick,
                                1u);
        (void)rc_sched_schedule(&control->sched, RC_EV_COIL_FIRE, cyl, fire_tick,
                                fire_tick + 2000u, 1u);
    }
    (void)generation;
    (void)dead_time_ms;
}

void rc_control_cycle(rc_control_t *control, uint32_t now_us)
{
    if (control == NULL || control->init_done == 0u) {
        return;
    }
    const rc_cal_t *cal = &control->cal;
    const rc_engine_t *engine = &control->engine;

    /* 1-2. refresh the position model and the sensor frame. */
    rc_trigger_update(&control->trigger, now_us);
    /* The watchdog, not the edge handler, is what catches a trigger that simply stopped.
     * Without it a dead signal would leave the ECU controlling the engine on a position it
     * no longer believes in. */
    (void)rc_trigger_check_timeout(&control->trigger, now_us);
    rc_trigger_get_position(&control->trigger, &control->state.pos);
    /* A wrap of the 720 degree counter is one engine cycle.  This is the clock the event
     * planner runs on; watching the angle value instead is ambiguous at the wrap point. */
    if (control->state.pos.cycle_angle_deg10 < control->last_cycle_angle) {
        control->cycle_count++;
    }
    control->last_cycle_angle = control->state.pos.cycle_angle_deg10;
    if (control_phase_permitted(control) == false) {
        control->state.armed = 0u;
    } else {
        control->state.armed = (uint8_t)(control->armed != 0u);
    }

    const float rpm = (float)control->state.pos.rpm_x2 * 0.5f;
    const float dt = 0.0005f; /* the 2 kHz fallback tick; the true value comes from the
                               * cycle interval when a 720 event drives the cycle */

    /* 3-4. load.  A failed primary sensor falls back to the next strategy in a documented
     * order, and the fallback is recorded rather than hidden. */
    const float map = sensor_or(control, RC_IN_MAP, 0.0f);
    const float tps = sensor_or(control, RC_IN_TPS1, 0.0f);
    float load = 0.0f;
    uint8_t load_source = (uint8_t)RC_LOAD_INVALID;
    if (sensor_ok(control, RC_IN_MAP)) {
        load = map;
        load_source = (uint8_t)RC_LOAD_MAP;
    } else if (sensor_ok(control, RC_IN_TPS1)) {
        load = tps;
        load_source = (uint8_t)RC_LOAD_TPS;
    }

    const float iat = sensor_or(control, RC_IN_IAT, 20.0f);
    const float ect = sensor_or(control, RC_IN_ECT, 80.0f);
    const float battery = sensor_or(control, RC_IN_BATTERY, 14.0f);
    const float fuel_pressure = sensor_or(control, RC_IN_FUEL_P, 300.0f);
    const float lambda = sensor_or(control, RC_IN_LAMBDA1, 1.0f);
    const bool lambda_valid = sensor_ok(control, RC_IN_LAMBDA1);

    /* Publish the readings themselves, not just what the fuel model used. The live-data
     * frame carries these fields, so anything left unset here is transmitted as a number
     * that looks like a measurement and is not one. */
    control->state.raw.tps_pct = tps;
    control->state.raw.iat_c = iat;
    control->state.raw.ect_c = ect;
    control->state.raw.battery_v = battery;
    control->state.raw.fuel_pressure_kpa = fuel_pressure;
    control->state.raw.oil_pressure_bar = sensor_or(control, RC_IN_OIL_P, 0.0f);
    control->state.raw.oil_temp_c = sensor_or(control, RC_IN_OIL_T, 0.0f);

    /* 16-18. performance features, as requests only. */
    rc_perf_input_t perf_in;
    memset(&perf_in, 0, sizeof(perf_in));
    perf_in.rpm = rpm;
    perf_in.tps_pct = tps;
    /* Gear position is NOT yet available: there is no gear input channel, and a GT650
     * needs one (a switch, or a wheel-speed-plus-rpm derivation) that has not been chosen
     * yet.  Register UNK-GT-020.
     *
     * This used to be fed `gear_reverse_detection_enabled`, a boolean configuration flag.
     * That is not a gear number: it silently supplied 0 or 1 depending on an unrelated
     * setting, which put the engine-braking table on the wrong row and the gear strategy
     * table on the wrong row, and neither would ever have looked wrong in a log.  The
     * honest value is a neutral gear 0, and the capability that depends on it is declared
     * unsupported rather than quietly wrong. */
    perf_in.gear = 0u;
    perf_in.dt_s = dt;
    rc_perf_output_t perf_out;
    rc_perf_update(&control->perf, cal, &control->torque, &perf_in, now_us, &perf_out);

    /* 19. safety is evaluated on the *measured* state, before it is allowed to influence
     * anything, and its verdict is a hard input to the fuel and ignition commands. */
    rc_safety_input_t safe_in;
    memset(&safe_in, 0, sizeof(safe_in));
    safe_in.rpm = rpm;
    safe_in.lambda = lambda;
    safe_in.lambda_valid = (uint8_t)(lambda_valid ? 1 : 0);
    safe_in.ect_c = ect;
    safe_in.ect_valid = (uint8_t)(sensor_ok(control, RC_IN_ECT) ? 1 : 0);
    safe_in.iat_c = iat;
    safe_in.iat_valid = (uint8_t)(sensor_ok(control, RC_IN_IAT) ? 1 : 0);
    safe_in.oil_pressure_bar = sensor_or(control, RC_IN_OIL_P, 0.0f);
    safe_in.oil_pressure_valid = (uint8_t)(sensor_ok(control, RC_IN_OIL_P) ? 1 : 0);
    safe_in.oil_temp_c = sensor_or(control, RC_IN_OIL_T, 20.0f);
    safe_in.oil_temp_valid = (uint8_t)(sensor_ok(control, RC_IN_OIL_T) ? 1 : 0);
    safe_in.egt_c = sensor_or(control, RC_IN_EGT, 0.0f);
    safe_in.egt_valid = (uint8_t)(sensor_ok(control, RC_IN_EGT) ? 1 : 0);
    safe_in.battery_v = battery;
    safe_in.battery_valid = (uint8_t)(sensor_ok(control, RC_IN_BATTERY) ? 1 : 0);
    safe_in.fuel_pressure_kpa = fuel_pressure;
    safe_in.fuel_pressure_valid = (uint8_t)(sensor_ok(control, RC_IN_FUEL_P) ? 1 : 0);
    safe_in.sync_lost = (uint8_t)(rc_trigger_permits_control(&control->trigger) ? 0u : 1u);
    safe_in.sensor_fault = (uint8_t)((control->sensors.fault_count > 0u) ? 1u : 0u);
    safe_in.sync_state = control->trigger.state;
    safe_in.boost_kpa = 0.0f;
    safe_in.speed_kph = 0.0f;

    /* The duty and dwell figures the envelope needs come from the previous cycle's
     * commands, which is honest: the envelope reacts to what is actually being commanded
     * rather than to a prediction of it. */
    safe_in.injector_duty_percent = control->state.corr.injector_model * 100.0f;
    safe_in.dwell_ms = control->state.dwell_ms[0];

    rc_safety_output_t safe_out;
    rc_safety_evaluate(&control->safety, cal, engine, &safe_in, now_us, &safe_out);
    rc_safety_service_latches(&control->safety, cal, &safe_in, now_us);

    /* 20. arbitration, then the fuel and ignition requests. */
    const float retard_per_unit = rc_cal_get_torque_retard_per_unit_torque_loss(cal);
    rc_torque_arbitrate(&control->torque, retard_per_unit);

    rc_cut_reason_t cut = RC_CUT_NONE;
    if (safe_out.fuel_cut != false) {
        cut = RC_CUT_SAFETY;
    } else if (cal->safety_sync_loss_action == 0u && safe_in.sync_lost != 0u) {
        cut = RC_CUT_SAFETY;
    } else if (rc_torque_fuel_forbidden(&control->torque) != false) {
        cut = RC_CUT_SAFETY;
    }

    rc_fuel_input_t fuel_in;
    memset(&fuel_in, 0, sizeof(fuel_in));
    fuel_in.rpm = rpm;
    fuel_in.load = load;
    fuel_in.load_source = load_source;
    fuel_in.map_kpa = map;
    fuel_in.baro_kpa = 100.0f;
    fuel_in.iat_c = iat;
    fuel_in.ect_c = ect;
    fuel_in.battery_v = battery;
    fuel_in.fuel_pressure_kpa = fuel_pressure;
    fuel_in.lambda = lambda;
    fuel_in.lambda_valid = (uint8_t)(lambda_valid ? 1 : 0);
    fuel_in.tps_pct = tps;
    fuel_in.dt_s = dt;
    fuel_in.crank_only = (safe_in.sync_lost != 0u);

    rc_fuel_output_t fuel_out;
    rc_fuel_update(&control->fuel, cal, engine, &fuel_in, cut,
                   control->torque.allowed_scale, safe_out.torque_factor, &fuel_out);

    /* Ignition. */
    rc_knock_output_t knock_out;
    rc_knock_publish(&control->knock, cal, &knock_out);
    rc_knock_decay(&control->knock, cal, dt);

    rc_idle_output_t idle_out;
    rc_idle_update(&control->idle, cal, rpm, rc_cal_get_engine_idle_rpm_target(cal), tps, ect,
                   control->torque.allowed_scale, now_us, &idle_out);

    rc_ignition_input_t ign_in;
    memset(&ign_in, 0, sizeof(ign_in));
    ign_in.rpm = rpm;
    ign_in.load = load;
    ign_in.iat_c = iat;
    ign_in.ect_c = ect;
    ign_in.baro_kpa = 100.0f;
    ign_in.battery_v = battery;
    ign_in.ethanol_fraction = rc_cal_get_engine_fuel_ethanol_fraction(cal);
    ign_in.tps_pct = tps;
    ign_in.dt_s = dt;
    ign_in.crank_only = safe_in.sync_lost != 0u;

    bool cut_flags[RC_MAX_CYLINDERS] = {false, false, false, false};
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        cut_flags[cyl] = (safe_out.spark_cut != false);
    }

    rc_ignition_output_t ign_out;
    rc_ignition_update(cal, engine, &knock_out, &ign_in, cut_flags, &ign_out);


    /* 21. the events that actually move the hardware, planned a full cycle ahead. */
    plan_cycle_events(control, now_us, engine, &fuel_out, &ign_out, &safe_out);

    /* 22. publish.  The envelope is applied to the *published* values, so the log and the
     * CAN frames carry the clamped command, never the pre-clamp one. */
    /* When the phase is not permitted the actuators get nothing, and the PUBLISHED state
     * must say so.  Publishing the computed fuel command while the hardware is being held
     * off is the worst kind of lie: a tuner reading a log would see a live fuel command on
     * an engine that is being deliberately prevented from running, and would go looking for
     * a fuel fault that does not exist. */
    const bool may_drive = control_phase_permitted(control) && (control->armed != 0u);
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        const bool live = may_drive && (cyl < engine->cylinder_count);
        control->state.injector_ms[cyl] = live ? fuel_out.base_pulse_ms : 0.0f;
        float angle = ign_out.final_deg[cyl] + idle_out.ignition_bis_deg
            - perf_out.engine_braking_retard_deg;
        angle = rc_clampf(angle, safe_out.ignition_min_deg, safe_out.ignition_max_deg);
        control->state.ignition_deg[cyl] = live ? angle : 0.0f;
        control->state.dwell_ms[cyl] = live ? ign_out.dwell_ms[cyl] : 0.0f;
        control->state.ignition_cut[cyl] = (uint8_t)(live ? ign_out.cut[cyl] : 1u);
    }
    control->state.idle_valve_pct = may_drive ? idle_out.valve_pct : 0.0f;

    control->state.sequence++;
    control->state.engine_time_us = now_us;
    control->state.load = load;
    control->state.load_source = load_source;
    control->state.baro_kpa = 100.0f;
    control->state.lambda[0] = lambda;
    control->state.lambda_valid[0] = (uint8_t)(lambda_valid ? 1 : 0);
    control->state.corr.base_map = fuel_out.base_pulse_ms;
    control->state.corr.sensor_corr = 1.0f;
    control->state.corr.transient = fuel_out.transient_pulse_ms;
    control->state.corr.closed_loop = fuel_out.closed_loop_factor;
    control->state.corr.cylinder_trim = fuel_out.cylinder_trim;
    control->state.corr.torque_allowance = control->torque.allowed_scale;
    control->state.corr.safety_factor = safe_out.torque_factor;
    control->state.arb_trace = control->torque.trace;
    control->state.safety_active = safe_out.active;
    control->state.action = safe_out.action;
    /* Re-publish armed, but only in the direction the cycle decided earlier.  Writing the
     * configured flag here unconditionally would undo the sync-loss disarm set at the top
     * of this function, and the published state would then claim the engine is armed while
     * the control path is in fact inhibited - a log that lies about the single most
     * important thing it knows. */
    if (control_phase_permitted(control) != false) {
        control->state.armed = (uint8_t)(control->armed != 0u);
    } else {
        control->state.armed = 0u;
    }

    /* 23. T1: withdraw every non-latching request at the end of the cycle. */
    rc_torque_end_cycle(&control->torque);
}

void rc_control_service(rc_control_t *control, uint32_t now_us, rc_hal_t *hal, void *hal_ctx)
{
    if (control == NULL) {
        return;
    }
    /* The HAL is a parameter rather than a compile-time singleton so the same control
     * system can drive real timer channels on the bench and the virtual engine in the
     * simulator.  Passing NULL is legal and means "no outputs are wired", which is what a
     * unit test that only inspects the published state wants - but a real ECU must pass
     * its own driver, or the scheduler will compute events that never reach an actuator. */
    rc_sched_dispatch(&control->sched, now_us, hal, hal_ctx);
    rc_sched_clear_expiry(&control->sched);
}

void rc_control_force_safe(rc_control_t *control, rc_hal_t *hal)
{
    if (control == NULL) {
        return;
    }
    control->armed = 0u;
    control->state.armed = 0u;
    for (uint8_t cyl = 0u; cyl < RC_MAX_CYLINDERS; ++cyl) {
        control->state.injector_ms[cyl] = 0.0f;
        control->state.ignition_cut[cyl] = 1u;
        control->state.dwell_ms[cyl] = 0.0f;
    }
    rc_sched_force_safe(&control->sched, hal, (void *)hal);
}
