/* RACCOON V2 - virtual engine plant model.  See rc_sim.h for what this is and is not. */
#include "rc_sim.h"

#include <math.h>
#include <string.h>

/* --- model constants.  Order of magnitude for a ~650 cc air-cooled twin, and explicitly
 * not measured.  They exist so the plant is stable and believable, not to be accurate. */
#define SIM_BAROMETERIC_KPA 101.3f
#define SIM_STOICH_AFR 14.7f
#define SIM_AIR_GAS_R 287.05f          /* J/(kg K) */
#define SIM_FUEL_DENSITY 745.0f        /* g/l, gasoline */

/* Friction and inertia.  A crank plus flywheel plus clutch is roughly 0.08..0.15 kg m^2 on
 * a bike of this size; the exact figure changes the transient, not the behaviour. */
#define SIM_INERTIA_KGM2 0.11f
#define SIM_FRICTION_NM_AT_1000RPM 3.0f
#define SIM_FRICTION_PER_RPM 0.0022f  /* viscous + pumping, linear in speed */
#define SIM_STARTER_NM 22.0f
#define SIM_IDLE_FLOOR_RPM 110.0f     /* below this the engine cannot sustain itself */
#define SIM_LOSS_OF_COMPRESSION_RPM 250.0f /* no combustion: the crank decays towards this */

/* Volumetric efficiency.  A flat base with a mild rpm falloff: enough character that a
 * closed-loop controller has something to correct, simple enough not to be a second ECU. */
static float sim_ve(float rpm)
{
    float ve = 0.70f;
    if (rpm > 1000.0f) {
        ve -= 0.10f * ((rpm - 1000.0f) / 9000.0f);
    }
    return (ve < 0.25f) ? 0.25f : ve;
}

/* Combustion efficiency against lambda.  Peak slightly rich of stoich, falling off hard
 * lean.  The lean cliff is the point: it is what a lean condition actually does to an
 * engine, and it is why the ECU has a lean cut. */
static float sim_combustion_efficiency(float lambda)
{
    if (lambda <= 0.0f) {
        return 0.0f;
    }
    if (lambda < 0.80f) {
        return 0.10f + (0.90f * (lambda / 0.80f));
    }
    if (lambda <= 1.05f) {
        return 1.0f;
    }
    const float over = (lambda - 1.05f) * 0.35f;
    return (over > 0.6f) ? 0.4f : (1.0f - over);
}

/** First-order lag toward a target: the standard way to model something that warms up. */
static float sim_lag(float current, float target, float tau_s, float dt_s)
{
    if (tau_s <= 0.0f) {
        return target;
    }
    const float k = dt_s / tau_s;
    return current + ((target - current) * (k / (1.0f + k)));
}

static float sim_clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void rc_sim_init(rc_sim_t *sim, float swept_cc_per_cylinder, uint8_t cylinders,
                 float compression_ratio)
{
    if (sim == NULL) {
        return;
    }
    memset(sim, 0, sizeof(*sim));
    sim->swept_cc = (swept_cc_per_cylinder > 0.0f) ? swept_cc_per_cylinder : 324.0f;
    sim->cylinders = (cylinders > 0u && cylinders <= RC_SIM_CYLINDERS) ? cylinders : 1u;
    sim->compression_ratio = (compression_ratio > 1.0f) ? compression_ratio : 9.5f;
    sim->baro_kpa = SIM_BAROMETERIC_KPA;
    sim->map_kpa = SIM_BAROMETERIC_KPA;
    sim->rpm = 0.0f;
    sim->running = false;
    /* A cold engine on a cold day.  CMT of 20 C is a normal cold-weather figure; the
     * sensors read it, so a warm-up scenario is observable from outside. */
    sim->iat_c = 15.0f;
    sim->ect_c = 20.0f;
    sim->oil_temp_c = 20.0f;
    sim->oil_pressure_bar = 0.0f;
    sim->egt_c = 20.0f;
    sim->lambda = 1.0f;
    sim->lambda_true = 1.0f;
    sim->afr_actual = SIM_STOICH_AFR;
}

void rc_sim_inject(rc_sim_t *sim, uint8_t cylinder, float pulse_ms, float dead_time_ms,
                   float flow_cc_per_s, float fuel_density_gasoline, float fuel_pressure_kpa)
{
    if (sim == NULL || cylinder >= RC_SIM_CYLINDERS) {
        return;
    }
    sim->injector_open_ms[cylinder] = pulse_ms;

    /* The single most important behaviour in this file.  A port injector that is not held
     * open long enough to overcome its own coil's inductance does not spray a little fuel:
     * it sprays NOTHING, while the ECU's log cheerfully shows a pulse.  Any test that does
     * not model this cannot detect a pulse-width bug, because the bug is invisible in the
     * model and lethal in the engine. */
    const float effective = pulse_ms - dead_time_ms;
    if (pulse_ms <= 0.0f || effective <= 0.0f) {
        sim->dead_time_pulses++;
        return;
    }
    if (!(flow_cc_per_s > 0.0f) || !(fuel_density_gasoline > 0.0f)) {
        return;
    }
    /* Volumetric flow falls with rail pressure, which is why a fuel pump fault makes an
     * engine run progressively lean rather than immediately stop. */
    const float g_per_s = (flow_cc_per_s * fuel_density_gasoline) / 1000.0f;
    const float pressure_scale = (fuel_pressure_kpa > 0.0f) ? (300.0f / fuel_pressure_kpa) : 0.0f;
    const float delivered_g = effective * 0.001f * g_per_s * pressure_scale;

    sim->fuel_delivered_g += delivered_g;
    sim->injector_pulses++;
    sim->fuel_mass_g_cycle += delivered_g;
}

float rc_sim_spark(rc_sim_t *sim, uint8_t cylinder, float angle_deg, float dwell_ms,
                   float iat_c, float load_pct)
{
    if (sim == NULL || cylinder >= RC_SIM_CYLINDERS) {
        return 0.0f;
    }
    sim->ignition_deg[cylinder] = angle_deg;
    sim->dwell_ms[cylinder] = dwell_ms;

    if (dwell_ms <= 0.0f) {
        return 0.0f;
    }
    sim->spark_count++;
    sim->fired_this_cycle = true;

    /* Knock margin.  Simplified on purpose: charge, temperature and advance.  This is a
     * stimulus for exercising the ECU's knock path, not a claim that knock is detected
     * here - a model that "detects" knock is a model of nothing. */
    const float charge = load_pct * 0.01f;
    const float heat = (iat_c - 20.0f) * 0.004f;
    const float advance_term = (angle_deg - 10.0f) * 0.010f;
    const float margin = 1.0f - (charge * 0.55f) - heat - advance_term;
    if (margin < 0.0f) {
        sim->knock_flag = true;
        sim->knock_margin = -margin;
    }
    return margin;
}

void rc_sim_step(rc_sim_t *sim, const rc_sim_rider_t *rider, const rc_sim_faults_t *faults,
                 float dt_s)
{
    static const rc_sim_faults_t no_faults;
    if (sim == NULL || rider == NULL || dt_s <= 0.0f) {
        return;
    }
    if (faults == NULL) {
        faults = &no_faults;
    }
    sim->step_count++;
    sim->now_us += (uint32_t)(dt_s * 1.0e6f);
    sim->fired_this_cycle = false;
    sim->last_crank_deg = sim->crank_angle_deg;

    /* ------------------------------------------------------------- throttle */
    float demand = sim_clampf(rider->throttle_pct, 0.0f, 100.0f);
    if (faults->tps_stuck_low) {
        demand = 2.0f;
    } else if (faults->tps_stuck_high) {
        demand = 98.0f;
    }
    /* Below ~10 % the throttle plate is down in the idle bypass, so MAP rises steeply. */
    /* A throttled engine pulls its manifold well below atmospheric: that restriction is
     * what makes an idle an idle.  The 10-90 % range below is the throttle plate, and the
     * plateau above 90 % is the engine breathing freely. */
    /* Closed throttle: a strong vacuum, roughly a third of atmospheric, which is the
     * restriction that makes an idle an idle.  Open throttle: the engine breathes freely
     * and the manifold reaches atmospheric.  The two regions are joined without a step,
     * because the throttle plate is a continuous restriction and a discontinuity here
     * would show up as a transient the ECU has to explain. */
    /* The closed-throttle figure is the one number that decides whether this plant can
     * idle at all, and it is a real constraint rather than a free parameter: a port
     * injector cannot meter less than its own dead time, so the smallest amount of fuel the
     * engine can be given is fixed.  If closed-throttle air exceeds what that minimum pulse
     * can fuel, the mixture is lean at idle by construction and the engine cannot be saved
     * by any control strategy - it simply dies.  Calibrated so the air at a closed throttle
     * is close to what a minimum pulse can support, which is the balance a real engine sits
     * at and the reason idle control exists at all. */
    const float frac = (demand <= 10.0f)
        ? (0.10f + (0.010f * demand))
        : (0.20f + (0.0080f * (demand - 10.0f)));
    float map_target = sim->baro_kpa * frac;
    if (faults->map_open) {
        /* An open MAP reads its faulted substitute, which for a simulated plant means the
         * ECU sees a plausible default rather than the truth. */
        map_target = sim->baro_kpa;
    }
    /* The idle air valve bypasses the throttle, so it adds charge the throttle plate is
     * not letting past.  This is not decoration: at idle the stoich charge is far below
     * what a port injector can actually meter - below its own dead time - so an engine can
     * only idle by being given MORE air than it needs.  Without this the plant is a model
     * of an engine that cannot idle, and every idle test is meaningless. */
    const float idle_air = sim->idle_valve_pct * 0.01f;
    map_target += sim->baro_kpa * (0.20f * idle_air);
    sim->map_kpa = sim_lag(sim->map_kpa, map_target, 0.010f, dt_s);

    /* --------------------------------------------------- charge and mixture */
    /* Ideal gas, with a bounded real-gas correction.  The ECU uses the same physics on
     * purpose: the interesting error for closed-loop testing is the ECU's CALIBRATION
     * being wrong, not its physics being wrong. */
    const float map_pa = sim->map_kpa * 1000.0f;
    const float iat_k = sim->iat_c + 273.15f;
    const float swept_m3 = sim->swept_cc * 1.0e-6f;
    const float ve = sim_ve(sim->rpm);
    const float volume_m3 = swept_m3 * ve;
    const float real_gas = 1.0f + (0.00035f * sim->map_kpa);
    const float air_mass_g = ((map_pa * volume_m3 * real_gas) / (SIM_AIR_GAS_R * iat_k))
        * 1000.0f;

    /* Mixture is a property of the CYLINDER, not of the time step.
     *
     *  Comparing the fuel injected during one step against the air inducted during that
     *  same step is meaningless, because at any real engine speed there are hundreds of
     *  steps between two injections: on most steps there is no fuel at all, so the
     *  "measured" AFR is whatever the ratio of a small number to zero happens to be.  The
     *  reading then depends on the step size, which is not a property of the engine.
     *
     *  So the fuel is accumulated over a whole crank revolution and compared with the air
     *  for that same revolution, which is the interval the ratio is actually defined over.
     *  The result is independent of how the step is chopped up. */
    const float fuel_g = sim->fuel_mass_g_cycle;
    sim->air_mass_g_cycle = air_mass_g;
    sim->cycle_fuel_g += fuel_g;

    /* Once per revolution, over an interval during which both terms are known. */
    if (sim->crank_angle_deg < sim->last_crank_deg) {
        const float air_cycle_g = air_mass_g;
        if (sim->cycle_fuel_g > 1.0e-9f && air_cycle_g > 1.0e-9f) {
            sim->afr_actual = air_cycle_g / sim->cycle_fuel_g;
        } else if (air_cycle_g > 1.0e-9f) {
            sim->afr_actual = 1.0e4f; /* air and no fuel: infinitely lean, not a division */
        } else {
            sim->afr_actual = SIM_STOICH_AFR; /* neither: nothing is burning */
        }
        sim->cycle_fuel_g = 0.0f;
    }
    sim->lambda_true = sim->afr_actual / SIM_STOICH_AFR;

    /* The wideband sees the mixture, slowly.  A real sensor has transport delay and
     * thermal inertia, so it must not be able to respond within one step. */
    float lambda_seen = sim->lambda_true;
    if (faults->lambda_noisy) {
        /* Deterministic wobble, not random: a reproducible test beats a flaky one, and the
         * ECU must survive a signal that is simply wrong. */
        lambda_seen += 0.05f * sinf((float)sim->step_count * 0.37f);
    }
    sim->lambda = sim_lag(sim->lambda, lambda_seen, 0.050f, dt_s);
    if (sim->lambda_true > 1.30f) {
        sim->lean_event_count += dt_s;
    }

    /* ------------------------------------------------------------ combustion */
    /* Fuel energy x how well it burns, turned into POWER and then into torque.
     *
     * The rate matters, and getting this wrong is what makes a plant model useless: the
     * fuel metered in this step is an amount, not a rate, and treating it as a rate makes
     * the produced power scale with the step size.  Halving the step would double the
     * torque.  So: grams per step -> grams per second -> joules per second -> newton metres
     * by dividing by the angular speed the engine is actually turning at.
     *
     * 46 MJ/kg is the lower heating value of petrol, roughly 10.4 kWh/kg. */
    const float efficiency = sim_combustion_efficiency(sim->lambda_true);
    const float fuel_rate_g_per_s = fuel_g / dt_s;
    const float burn_power_w = fuel_rate_g_per_s * 1.0e-3f * 46.0e6f * efficiency;
    float burn_torque_nm = 0.0f;
    if (sim->omega_rad_s > 1.0f) {
        burn_torque_nm = burn_power_w / sim->omega_rad_s;
    } else {
        /* At or below walking pace there is no meaningful power-to-torque ratio, and a
         * near-zero divisor would produce an enormous torque.  An engine being cranked
         * makes its torque from compression, which this model does not simulate, so the
         * honest answer here is no combustion torque. */
        burn_torque_nm = 0.0f;
    }
    /* A stalled or barely turning engine cannot make torque from a fire it is not having. */
    if (sim->rpm < 150.0f) {
        burn_torque_nm = 0.0f;
    }

    /* ------------------------------------------------------------ dynamics */
    /* Everything that accelerates or decelerates the crank, in one place and with the
     * signs written the same way round: torques that SPEED THE ENGINE UP are added, torques
     * that slow it down are subtracted.  A starter is a source, not a bigger friction
     * load, and getting that backwards means a cranking engine never turns over - which
     * looks exactly like a dead ECU. */
    float friction_nm = SIM_FRICTION_NM_AT_1000RPM + (SIM_FRICTION_PER_RPM * sim->rpm);
    if (!sim->running) {
        /* With no combustion the crank is dragged down by friction and pumping alone. */
        friction_nm += 6.0f;
    }
    const float starter_nm = rider->starter_engaged ? SIM_STARTER_NM : 0.0f;

    /* Road load.  Aero drag plus rolling resistance plus whatever the clutch is doing. */
    float road_nm = 0.0f;
    if (!rider->clutch_engaged && rider->gear > 0u) {
        const float wheel_nm = (sim->speed_kph * 0.05f)
            + (sim->speed_kph * sim->speed_kph * 0.0022f);
        road_nm = wheel_nm / 12.0f; /* a tall first gear stands in for the whole train */
    }
    /* Brakes act on the WHEELS, so with the clutch out they load the engine through the
     * gearing.  This is engine braking, and it is strong: a motorcycle at 4000 rpm with
     * the front brake on is decelerating hard.  Without it a closed-throttle engine in
     * this plant never stops, and a stall scenario cannot be written at all. */
    if (rider->brake_pct > 0.0f) {
        road_nm += (rider->brake_pct * 0.01f) * 60.0f;
    }
    /* Closed throttle is also engine braking, through the exhaust and the pumping losses. */
    if (rider->throttle_pct < 2.0f && sim->running) {
        road_nm += 4.0f;
    }

    const float net_nm = burn_torque_nm + starter_nm - friction_nm - road_nm;
    const float domega = (net_nm / SIM_INERTIA_KGM2) * dt_s;
    /* Below a stop the crank does not drive backwards: a stalled motorcycle engine coasts
     * to rest and stays there, and a model that integrates it negative invents a
     * reverse-running engine. */
    const float next_omega = sim->omega_rad_s + domega;
    sim->omega_rad_s = (next_omega < 0.0f) ? 0.0f : next_omega;
    sim->rpm = (sim->omega_rad_s * 60.0f) / (2.0f * 3.14159265f);
    if (sim->rpm > sim->peak_rpm) {
        sim->peak_rpm = sim->rpm;
    }
    /* The crank angle is the plant's own state.  It belongs here, not in the harness: a
     * caller that advanced it would have to duplicate the speed integration, and the
     * engine would then have two crankshafts that agree only by accident. */
    sim->crank_angle_deg = fmodf(sim->crank_angle_deg + (sim->rpm * dt_s * 6.0f), 720.0f);

    /* Running or stalled.  Combustion above the idle floor sustains the engine; below it,
     * with no starter, the engine has stopped.  Nothing else decides this. */
    const bool combusting = (efficiency > 0.25f) && sim->fired_this_cycle
        && (sim->rpm > SIM_IDLE_FLOOR_RPM);
    if (combusting) {
        if (!sim->running) {
            sim->running = true;
        }
    } else if (!rider->starter_engaged && sim->rpm < SIM_LOSS_OF_COMPRESSION_RPM) {
        if (sim->running) {
            sim->stall_count += 1.0f;
        }
        sim->running = false;
    }
    if (rider->starter_engaged) {
        sim->running = true;
    }

    /* --------------------------------------------------------- temperatures */
    const float load_frac = (air_mass_g / 0.05f);
    const float ect_target = 80.0f + (140.0f * sim_clampf(load_frac, 0.0f, 1.2f))
        - (12.0f * ((sim->rpm > 2000.0f) ? 1.0f : 0.0f));
    sim->ect_c = sim_lag(sim->ect_c, (sim->running ? ect_target : 20.0f), 12.0f, dt_s);
    const float egt_target = 200.0f + (750.0f * sim_clampf(load_frac, 0.0f, 1.2f));
    sim->egt_c = sim_lag(sim->egt_c, (sim->running ? egt_target : 20.0f), 6.0f, dt_s);
    const float oil_target = 30.0f + (70.0f * sim_clampf(load_frac, 0.0f, 1.0f));
    sim->oil_temp_c = sim_lag(sim->oil_temp_c, oil_target, 90.0f, dt_s);
    /* Oil pressure is a positive-displacement pump: it rises steeply with speed and is
     * near zero when the engine is not turning.  This is what makes a stall trip the oil
     * protection, and why that protection has a hold time. */
    const float oil_target_p = (sim->rpm / 1000.0f) * 2.6f;
    sim->oil_pressure_bar = sim_lag(sim->oil_pressure_bar, oil_target_p, 0.05f, dt_s);

    /* ----------------------------------------------------------- drivetrain */
    if (!rider->clutch_engaged && rider->gear > 0u) {
        const float target_kph = (sim->rpm / 6.0f) * ((float)rider->gear) * 3.0f;
        sim->speed_kph = sim_lag(sim->speed_kph, target_kph, 0.30f, dt_s);
    } else {
        sim->speed_kph = sim_lag(sim->speed_kph, 0.0f, 0.60f, dt_s);
    }
    sim->wheel_speed_kph = sim->speed_kph;
    sim->rear_wheel_kph = sim->speed_kph;
    /* The front wheel is on the ground and only spins if it is rolling. */
    sim->front_wheel_kph = sim->speed_kph;

    /* -------------------------------------------------------- housekeeping */
    sim->fuel_mass_g_cycle = 0.0f; /* metered per step, not accumulated */
    if (faults->oil_sensor_stuck) {
        sim->oil_pressure_bar = 2.0f; /* frozen at a plausible value forever */
    }
    sim->knock_flag = false;
    sim->knock_margin = 0.0f;
}

float rc_sim_next_ckp_angle(const rc_sim_t *sim, uint8_t teeth, uint8_t missing)
{
    if (sim == NULL || teeth == 0u) {
        return 0.0f;
    }
    /* Teeth are evenly spaced over the revolution; the missing ones are a gap at the end of
     * the pattern, which is what a real wheel looks like. */
    const float span = 360.0f / (float)teeth;
    const float present = (float)(teeth - (missing < teeth ? missing : 0u));
    const float position = fmodf(sim->crank_angle_deg, 360.0f) / span;
    const uint32_t next_index = (uint32_t)floorf(position) + 1u;
    if (missing == 0u) {
        return fmodf((float)next_index * span, 360.0f);
    }
    if (next_index >= (uint32_t)teeth) {
        return 0.0f; /* the gap: the next edge is a full period away */
    }
    (void)present;
    return (float)next_index * span;
}

float rc_sim_time_to_next_ckp(const rc_sim_t *sim, uint8_t teeth, uint8_t missing)
{
    const float next_deg = rc_sim_next_ckp_angle(sim, teeth, missing);
    float delta = next_deg - sim->crank_angle_deg;
    /* The gap is not a normal tooth interval: the edge after the gap is a full period on. */
    if (missing > 0u) {
        const float span = 360.0f / (float)((teeth > 0u) ? teeth : 1u);
        const float position = fmodf(sim->crank_angle_deg, 360.0f) / span;
        const uint32_t index = (uint32_t)floorf(position);
        if (index >= (uint32_t)(teeth - missing)) {
            delta = (360.0f - fmodf(sim->crank_angle_deg, 360.0f)) + next_deg;
        }
    }
    if (delta < 0.0f) {
        delta += 360.0f;
    }
    if (sim->rpm <= 0.01f) {
        return 1.0e9f; /* stopped: the next edge is not on a schedule */
    }
    /* degrees -> seconds at the current speed. */
    return (delta / 360.0f) * (60.0f / sim->rpm);
}

float rc_sim_time_to_next_cmp(const rc_sim_t *sim, uint8_t cam_teeth, float *angle_out)
{
    if (sim == NULL || cam_teeth == 0u) {
        if (angle_out != NULL) {
            *angle_out = 0.0f;
        }
        return 1.0e9f;
    }
    /* A cam wheel with n teeth produces n edges per crank revolution, and on a four-stroke
     * they repeat once per cycle, so the pattern is simply evenly spaced over 360 degrees. */
    const float span = 360.0f / (float)cam_teeth;
    const float position = fmodf(sim->crank_angle_deg, 360.0f) / span;
    const float next_deg = fmodf((floorf(position) + 1.0f) * span, 360.0f);
    if (angle_out != NULL) {
        *angle_out = next_deg;
    }
    float delta = next_deg - fmodf(sim->crank_angle_deg, 360.0f);
    if (delta <= 0.0f) {
        delta += 360.0f;
    }
    if (sim->rpm <= 0.01f) {
        return 1.0e9f;
    }
    return (delta / 360.0f) * (60.0f / sim->rpm);
}
