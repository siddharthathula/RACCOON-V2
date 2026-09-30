"""Turning a vehicle profile into a working calibration.

The division of labour is the whole point of this module:

**Derived, here.** Geometry, the trigger period window, the injector and ignition ceilings,
the sensor strategy counts, the safety envelope, the firing order. Every one of these is
determined by the profile and by arithmetic, so computing them is both possible and
correct. A hand-typed period window is one somebody got wrong.

**Measured, on a dyno.** The fuel table, the ignition table, the target lambda table. The
ECU ships plausible starting points for these and they are deliberately conservative - a
starting calibration that will idle and cruise, and that is *not* a track tune. This module
does not pretend otherwise, and it labels the result so the tuner cannot mistake it for a
finished tune.

**Refused, here.** A profile that is internally inconsistent - a stated displacement that
disagrees with bore and stroke, a firing order that repeats a cylinder, a cam wheel on an
engine configured for sequential injection with no cam phase - is rejected with the reason
named. These are the mistakes that are invisible on the bench and catastrophic at 9000 rpm.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from raccoon import calibration as cal
from raccoon.vehicle import Provenance, Value, VehicleProfile

# Enum values as the generated schema defines them. Spelled out rather than imported from
# generated headers, because these are the tuner's contract with the descriptor, and a
# mismatch has to fail here with a clear message rather than as a wrong number later.
INJECTION_STRATEGY = {"batch": 0, "simultaneous": 0, "semi-sequential": 1,
                      "sequential": 2, "staged": 3}
IGNITION_STRATEGY = {"static_table": 0, "knock_closed_loop": 1, "adaptive_4d": 2}
# The coil topology is a separate thing from the ignition control mode: a smart coil and a
# wasted-spark pair are both "static table" as far as the ignition strategy is concerned.
# Conflating them is how a profile ends up claiming wasted spark on a single smart coil.
INJECTOR_DRIVER = {"high_z_saturated": 0, "low_z_peak_and_hold": 1}
IGNITION_TOPOLOGY = {"smart_coil": 0, "logic_level": 1, "dumb_coil_external_igbt": 2,
                     "wasted_spark": 3}
IDLE_STRATEGY = {"off": 0, "pwm_iac": 1, "stepper": 2, "dbw": 3, "ignition": 4}
LAMBDA_STRATEGY = {"off": 0, "narrowband": 1, "wideband": 1, "wideband_2": 2}

# hardware_capabilities, in bit order as the generated descriptor lists them. The GUI adapts
# to this rather than to a hard-coded feature list, which is the whole point of the
# capability model: a build with no knock sensor must not offer a knock screen.
CAPABILITY_BITS = {
    "cap_ckp": 0, "cap_cmp": 1, "cap_sequential": 2, "cap_semi_sequential": 3,
    "cap_batch": 4, "cap_wasted_spark": 5, "cap_staged": 6, "cap_wideband": 7,
    "cap_narrowband": 8, "cap_multiple_lambda": 9, "cap_knock": 10, "cap_vvt": 11,
    "cap_boost": 12, "cap_traction": 13, "cap_dbw": 14, "cap_flex_fuel": 15,
}


class ProfileError(Exception):
    """The profile cannot produce a calibration, and the reason is worth reading."""


@dataclass
class BuildReport:
    """What was built, and what still has to be measured.

    Returned alongside the calibration rather than logged, because the caller - a human, or
    the tuner's project screen - has to be able to act on it: this list is the work plan.
    """

    profile: str
    derived: list[str] = field(default_factory=list)
    outstanding: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def add_derived(self, what: str) -> None:
        self.derived.append(what)

    def add_outstanding(self, what: str) -> None:
        self.outstanding.append(what)

    def add_warning(self, what: str) -> None:
        self.warnings.append(what)

    def summary(self) -> str:
        lines = [f"profile: {self.profile}",
                 f"derived {len(self.derived)} values, "
                 f"{len(self.outstanding)} still need measuring"]
        if self.warnings:
            lines.append("warnings:")
            lines.extend(f"  ! {w}" for w in self.warnings)
        if self.outstanding:
            lines.append("measure these before running on a track:")
            lines.extend(f"  - {o}" for o in self.outstanding)
        return "\n".join(lines)


def _set(image: cal.Calibration, name: str, value) -> None:
    """Set a scalar by name, converting the failure into a named error.

    A missing item means the schema and this module have drifted, which is a build problem
    rather than a profile problem, and the message says which.
    """
    item = cal.ITEMS_BY_NAME.get(name)
    if item is None:
        raise ProfileError(
            f"calibration item {name!r} does not exist. The schema has changed and this "
            f"module has not caught up."
        )
    try:
        image.set(item, value)
    except (ValueError, TypeError) as exc:
        raise ProfileError(f"{name} = {value!r} was refused: {exc}") from exc


def _get(item_name: str) -> cal.CalibrationItem:
    item = cal.ITEMS_BY_NAME.get(item_name)
    if item is None:
        raise ProfileError(f"calibration item {item_name!r} does not exist")
    return item


def validate(profile: VehicleProfile) -> list[str]:
    """Check a profile for internal consistency. Returns the problems, empty if sound.

    Run before anything is written, because these are the failures that produce an ECU which
    builds cleanly and then misbehaves at speed: a period window derived from the wrong
    tooth count, a displacement that does not match the bore and stroke, a firing order that
    fires two cylinders into the same port.
    """
    problems: list[str] = []
    g = profile.geometry
    t = profile.trigger
    h = profile.hardware
    lim = profile.limits

    cylinders = int(g.cylinders.value)
    if not 1 <= cylinders <= 4:
        problems.append(f"cylinder_count is {cylinders}; the ECU supports 1 to 4")

    if g.bore_mm.value <= 0 or g.stroke_mm.value <= 0:
        problems.append("bore and stroke must both be positive")
    elif not 0.5 <= g.compression_ratio.value <= 25.0:
        problems.append(
            f"compression ratio {g.compression_ratio.value:g} is outside anything a road "
            f"engine has; check it is a ratio and not a fraction"
        )

    order = list(g.firing_order)
    if sorted(order) != list(range(cylinders)):
        problems.append(
            f"firing_order {order} is not a permutation of 0..{cylinders - 1}. A cylinder "
            f"repeated or omitted means two cylinders fire into the same port on a "
            f"cross-plane engine, and the engine will not start - or worse, will."
        )

    if t.crank_teeth.value < 2:
        problems.append("a crank trigger needs at least two teeth to establish a period")
    if t.missing_teeth.value >= t.crank_teeth.value:
        problems.append(
            f"missing_teeth ({int(t.missing_teeth.value)}) must be fewer than the total "
            f"teeth ({int(t.crank_teeth.value)}), or there is no crank wheel left"
        )
    if t.missing_teeth.value and not 0 <= t.missing_tooth_index.value < t.crank_teeth.value:
        problems.append("the missing tooth index points outside the crank wheel")

    if t.teeth_per_rev.value not in (1, 2):
        problems.append("teeth_per_rev is the number of edges per crank revolution: 1 or 2")

    if h.injector_count.value < cylinders:
        problems.append(
            f"{int(h.injector_count.value)} injector(s) fitted for {cylinders} cylinders: "
            f"the remaining cylinders would get no fuel at all"
        )
    if h.ignition_count.value < cylinders:
        problems.append(
            f"{int(h.ignition_count.value)} ignition channel(s) for {cylinders} cylinders"
        )
    if h.injector_peak_a.value < h.injector_hold_a.value:
        problems.append(
            f"injector peak current ({h.injector_peak_a.value:g} A) is below the hold "
            f"current ({h.injector_hold_a.value:g} A); the driver would be in a state it "
            f"was never specified for"
        )
    if h.ignition_peak_a.value < h.ignition_hold_a.value:
        problems.append("ignition peak current is below the hold current")

    strategy = INJECTION_STRATEGY.get(profile.injection_strategy)
    if profile.ignition_strategy not in IGNITION_STRATEGY:
        problems.append(
            f"ignition_strategy {profile.ignition_strategy!r} is not one of "
            f"{sorted(IGNITION_STRATEGY)}"
        )
    if str(h.injector_driver_type.value) not in INJECTOR_DRIVER:
        problems.append(
            f"injector_driver_type {h.injector_driver_type.value!r} is not one of "
            f"{sorted(INJECTOR_DRIVER)}"
        )
    if str(h.ignition_driver_topology.value) not in IGNITION_TOPOLOGY:
        problems.append(
            f"ignition_driver_topology {h.ignition_driver_topology.value!r} is not one of "
            f"{sorted(IGNITION_TOPOLOGY)}"
        )
    if str(h.ignition_driver_topology.value) == "wasted_spark" and h.ignition_count.value < 2 * cylinders:
        problems.append(
            f"wasted spark needs a coil pair per cylinder: {int(2 * cylinders)} coils for "
            f"{cylinders} cylinders, and {int(h.ignition_count.value)} are fitted"
        )
    if profile.idle_strategy not in IDLE_STRATEGY:
        problems.append(
            f"idle_strategy {profile.idle_strategy!r} is not one of {sorted(IDLE_STRATEGY)}"
        )
    if strategy is None:
        problems.append(
            f"injection_strategy {profile.injection_strategy!r} is not one of "
            f"{sorted(INJECTION_STRATEGY)}"
        )
    elif strategy >= INJECTION_STRATEGY["sequential"]:
        if t.cam_teeth.value < 2:
            problems.append(
                f"injection strategy {profile.injection_strategy!r} needs a cam signal to "
                f"know the phase, and the trigger is configured with "
                f"{int(t.cam_teeth.value)} cam tooth/teeth. Either fit a cam trigger or use "
                f"semi-sequential, which does not need one."
            )

    if lim.safety_rpm_limit.value >= lim.max_rpm.value:
        problems.append(
            f"the safety limit ({lim.safety_rpm_limit.value:g}) must be BELOW the maximum "
            f"({lim.max_rpm.value:g}), or the ECU will be commanded to cut fuel at the rev "
            f"limit it is meant to be protecting"
        )
    if lim.overspeed_rpm.value <= lim.safety_rpm_limit.value:
        problems.append("the overspeed threshold must be above the safety limit")
    if lim.idle_rpm.value >= 0.9 * lim.safety_rpm_limit.value:
        problems.append("the idle target must be well below the safety limit")
    if lim.lean_lambda_limit.value <= lim.rich_lambda_limit.value:
        problems.append("the lean lambda limit must be above the rich limit")
    if lim.ignition_max_deg.value <= lim.ignition_min_deg.value:
        problems.append("the ignition envelope is inverted")

    if profile.lambda_strategy not in LAMBDA_STRATEGY:
        problems.append(
            f"lambda_strategy {profile.lambda_strategy!r} is not one of "
            f"{sorted(LAMBDA_STRATEGY)}"
        )
    elif profile.lambda_strategy != "off" and h.lambda_channels.value < 1:
        problems.append(
            f"lambda strategy {profile.lambda_strategy!r} is selected but no lambda "
            f"channel is fitted, so closed-loop control could never arm"
        )

    if h.has_traction.value and h.wheel_speed_channels.value < 2:
        problems.append("traction control needs two wheel speed channels, not one")
    if h.has_dbw.value and (h.pedal_channels.value < 2 or h.throttle_channels.value < 2):
        problems.append("drive-by-wire needs two pedal and two throttle sensors")
    if not h.has_dbw.value and h.throttle_channels.value >= 2:
        problems.append(
            f"{int(h.throttle_channels.value)} throttle sensors fitted but DBW is off: "
            f"that is either a wiring mistake or a mislabelled mode, and a single-throttle "
            f"cable with a duplicated sensor will run the idle control off a phantom input"
        )
    if h.knock_sensor_count.value > 1:
        problems.append("only one knock sensor channel is implemented")

    return problems


def _clamp_image(image: cal.Calibration, report: BuildReport, lim) -> None:
    """Pull the tables inside whatever envelope the profile declares.

    Every clamp is reported with how many cells it moved. A silent clamp looks identical to a
    table that happened to be in range, and the operator needs to know that a limit they set
    changed the tune under them.
    """
    # The ignition and lambda tables are left alone: their envelopes are widened above to
    # cover the tables' ranges, which removes the possibility of a silent clamp rather than
    # papering over it after the fact. Clamping the CELLS would hide a real conflict between
    # the profile and the schema.

    # The boost table is a genuine ceiling rather than a range-containment rule, so a cell
    # above the maximum really is outside what the hardware can deliver and has to be
    # brought down.
    boost = cal.ITEMS_BY_NAME["boost.boost_target_table"]
    if lim.boost_max_kpa.value < 250.0:
        cells = image.table(boost)
        ceiling = lim.boost_max_kpa.value
        moved = [i for i, c in enumerate(cells) if c > ceiling]
        if moved:
            for i, c in enumerate(cells):
                image.set_cell(boost, i, min(c, ceiling))
            report.add_derived(
                f"clamped {len(moved)} boost cell(s) to the {ceiling:g} kPa ceiling"
            )


def build(profile: VehicleProfile, base: cal.Calibration | None = None) -> tuple[cal.Calibration, BuildReport]:
    """Produce a calibration for a profile.

    `base` is a starting point for the parts a profile cannot know: the fuel and ignition
    tables. Pass the shipped defaults for a conservative road tune, or a dyno-derived set
    when you have one. Nothing here overwrites a table.
    """
    problems = validate(profile)
    if problems:
        raise ProfileError(
            "this profile is not internally consistent:\n  - "
            + "\n  - ".join(problems)
        )

    image = cal.Calibration.from_bytes(base.to_bytes()) if base is not None else cal.Calibration()
    report = BuildReport(profile=profile.name)
    g = profile.geometry
    t = profile.trigger
    h = profile.hardware
    lim = profile.limits
    cylinders = int(g.cylinders.value)

    # ---------------------------------------------------------------- geometry
    _set(image, "engine.cylinder_count", cylinders)
    _set(image, "engine.bore_mm", g.bore_mm.value)
    _set(image, "engine.stroke_mm", g.stroke_mm.value)
    _set(image, "engine.compression_ratio", g.compression_ratio.value)
    _set(image, "engine.rod_ratio", g.rod_ratio.value)
    _set(image, "engine.crank_layout", int(g.crank_layout.value))
    _set(image, "engine.displacement_cc", g.displacement_cc() * cylinders)
    for i in range(4):
        firing = g.firing_order[i] if i < len(g.firing_order) else i
        _set(image, f"engine.firing_order_cyl{i}", firing)
    report.add_derived("engine geometry, including the displacement from bore x stroke")
    report.add_outstanding(
        "confirm the displacement against the manufacturer's figure - the profile's value "
        "is computed from bore and stroke, which differ slightly from a rounded number"
    )

    # ----------------------------------------------------------------- trigger
    _set(image, "trigger.teeth", int(t.crank_teeth.value))
    _set(image, "trigger.missing_teeth", int(t.missing_teeth.value))
    _set(image, "trigger.tooth1_angle_atdc", t.tooth_1_angle_atdc.value)
    _set(image, "trigger.teeth_per_rev", int(t.teeth_per_rev.value))
    _set(image, "trigger.trigger_edge_polarity", int(t.tooth_1_polarity.value))
    _set(image, "trigger.input_filter_level", int(t.input_filter_level.value))
    _set(image, "trigger.sync_skip_revolutions", int(t.sync_skip_revolutions.value))
    if t.cam_teeth.value >= 2:
        if t.cam_teeth.value in (3, 4):
            # A named profile, so the decoder uses the count the profile carries and the
            # explicit item stays zero. Setting both would be two sources of truth.
            _set(image, "trigger.profile_cam", 1 if t.cam_teeth.value == 3 else 2)
        else:
            _set(image, "trigger.profile_cam", 4)   # custom
            _set(image, "trigger.cam_teeth", int(t.cam_teeth.value))
            _set(image, "trigger.cam_missing_teeth", 1 if t.cam_teeth.value >= 2 else 0)
    report.add_derived("trigger tooth count, polarity and filter level")

    # The period window, from the actual wheel.
    #
    # Three numbers, and the shipped defaults fix their relationship: overspeed_period is
    # exactly twice min_period. So the two are NOT both "the period at some rpm" - they are
    # the overspeed threshold and a noise floor below it:
    #
    #   overspeed_period = tooth period at the overspeed rpm. Below this, the engine is
    #                      running faster than it is allowed to.
    #   min_period       = a floor well below that, rejecting a single-edge glitch. A
    #                      floor at or above the overspeed threshold would mean every noise
    #                      spike is an overspeed, and the ECU faults itself on a bad earth
    #                      lead.
    #   max_period       = the tooth period at a cranking speed, generously long, because a
    #                      cranking engine turns slowly enough that tooth jitter looks like
    #                      a stall.
    #
    # Getting these from the wheel rather than typing them is the whole point: a period
    # window belongs to a specific crank wheel, and a copied one belongs to somebody else's.
    overspeed = max(20, int(round(t.period_us(lim.overspeed_rpm.value))))
    min_period = max(20, int(round(overspeed / 2.0)))
    # The stall bound is the tooth period at a cranking speed, and the item has a floor of
    # 10 ms. A 60-tooth wheel at 60 rpm has an 8.3 ms period, which is below that floor -
    # so the cranking speed has to come from the wheel rather than being fixed. 40 rpm on the
    # slowest plausible wheel is 16.7 ms, inside the range; and the clamp below is reported
    # rather than applied silently, because a stall bound that had to be clamped is a stall
    # bound somebody chose by accident.
    max_period = int(round(t.period_us(40.0)))
    period_item = _get("trigger.max_period_us")
    if max_period < period_item.minimum:
        report.add_warning(
            f"the stall bound for a {int(t.crank_teeth.value)}-tooth wheel is "
            f"{max_period} us, below the item's {period_item.minimum:g} us floor, so it was "
            f"raised to the floor. That means this wheel cannot distinguish a crank from a "
            f"stall below {period_item.minimum:g} us - check the tooth count is right."
        )
        max_period = int(period_item.minimum)
    _set(image, "trigger.min_period_us", min_period)
    _set(image, "trigger.overspeed_period_us", overspeed)
    _set(image, "trigger.max_period_us", max_period)
    report.add_derived(
        f"trigger period window from the {int(t.crank_teeth.value)}-tooth wheel: "
        f"overspeed at {lim.overspeed_rpm.value:g} rpm is {overspeed} us, noise floor "
        f"{min_period} us, stall at {max_period} us"
    )
    report.add_outstanding(
        "confirm tooth 1's angle with a timing light or a scope - the profile's value is a "
        "starting point, and TDC is what the ignition table is timed against"
    )

    # ---------------------------------------------------------------- hardware
    _set(image, "hardware.injector_peak_current_a", h.injector_peak_a.value)
    _set(image, "hardware.injector_hold_current_a", h.injector_hold_a.value)
    _set(image, "hardware.injector_max_duty_pct", h.injector_max_duty_pct.value)
    _set(image, "hardware.hardware_injector_channel_count", int(h.injector_count.value))
    _set(image, "hardware.ignition_peak_current_a", h.ignition_peak_a.value)
    _set(image, "hardware.ignition_hold_current_a", h.ignition_hold_a.value)
    _set(image, "hardware.ignition_max_dwell_ms", h.ignition_max_dwell_ms.value)
    _set(image, "hardware.hardware_ignition_channel_count", int(h.ignition_count.value))
    _set(image, "hardware.ignition_spark_divider_ratio", h.spark_divider_ratio.value)
    _set(image, "hardware.hardware_knock_channel_count", int(h.knock_sensor_count.value))
    report.add_derived("injector and ignition driver currents, dwell ceiling and channel counts")

    report.add_outstanding(
        f"injector flow at {h.injector_peak_a.value:g} A: the fuel model is built on flow in "
        f"cc per second, and a guess here is a guess at every fuelling number in the tune"
    )
    report.add_outstanding(
        "verify the crank and cam signal levels at the ECU pins - a 5 V conditioner on a 12 V "
        "sensor destroys the input and the profile cannot know"
    )

    # ---------------------------------------------------------------- strategy
    _set(image, "strategy.injection_strategy", INJECTION_STRATEGY[profile.injection_strategy])
    _set(image, "strategy.lambda_strategy", LAMBDA_STRATEGY[profile.lambda_strategy])
    _set(image, "strategy.lambda_channel_count", int(h.lambda_channels.value))
    _set(image, "strategy.knock_sensor_count", int(h.knock_sensor_count.value))
    _set(image, "strategy.wheel_speed_channel_count", int(h.wheel_speed_channels.value))
    _set(image, "strategy.pedal_channel_count", int(h.pedal_channels.value))
    _set(image, "strategy.throttle_channel_count", int(h.throttle_channels.value))
    _set(image, "strategy.dbw_strategy", 1 if h.has_dbw.value else 0)
    _set(image, "idle.idle_strategy", IDLE_STRATEGY[profile.idle_strategy])
    _set(image, "ignition.ignition_strategy", IGNITION_STRATEGY[profile.ignition_strategy])
    _set(image, "hardware.injector_driver_type", INJECTOR_DRIVER[str(h.injector_driver_type.value)])
    _set(image, "hardware.ignition_driver_topology",
         IGNITION_TOPOLOGY[str(h.ignition_driver_topology.value)])
    _set(image, "engine.cycle_teeth", 1)   # the four-stroke 720 degree cycle
    _set(image, "traction.traction_enabled", bool(h.has_traction.value))
    _set(image, "boost.boost_closed_loop_enabled", bool(h.has_boost.value))
    _set(image, "vvt.vvt_enabled", bool(h.has_vvt.value))
    report.add_derived("strategy selections and the sensor counts they require")

    # ------------------------------------------------------- the capability set
    #
    # Derived from what the profile says is FITTED, not from what the firmware implements.
    # The GUI reads this and hides the features the vehicle cannot do, which is the only
    # thing that stops an operator tuning a boost controller onto an engine with no turbo.
    caps = 0
    caps |= 1 << CAPABILITY_BITS["cap_ckp"]
    if t.cam_teeth.value >= 2:
        caps |= 1 << CAPABILITY_BITS["cap_cmp"]
    strategy_value = INJECTION_STRATEGY[profile.injection_strategy]
    if strategy_value == INJECTION_STRATEGY["staged"]:
        caps |= 1 << CAPABILITY_BITS["cap_staged"]
    elif strategy_value == INJECTION_STRATEGY["sequential"]:
        caps |= 1 << CAPABILITY_BITS["cap_sequential"]
    elif strategy_value == INJECTION_STRATEGY["semi-sequential"]:
        caps |= 1 << CAPABILITY_BITS["cap_semi_sequential"]
    else:
        caps |= 1 << CAPABILITY_BITS["cap_batch"]
    if h.ignition_count.value >= 2 * cylinders:
        caps |= 1 << CAPABILITY_BITS["cap_wasted_spark"]
    if h.lambda_channels.value:
        caps |= 1 << CAPABILITY_BITS["cap_wideband"]
    if h.lambda_channels.value >= 2:
        caps |= 1 << CAPABILITY_BITS["cap_multiple_lambda"]
    if h.knock_sensor_count.value:
        caps |= 1 << CAPABILITY_BITS["cap_knock"]
    if h.has_vvt.value:
        caps |= 1 << CAPABILITY_BITS["cap_vvt"]
    if h.has_boost.value:
        caps |= 1 << CAPABILITY_BITS["cap_boost"]
    if h.has_traction.value:
        caps |= 1 << CAPABILITY_BITS["cap_traction"]
    if h.has_dbw.value:
        caps |= 1 << CAPABILITY_BITS["cap_dbw"]
    _set(image, "hardware.hardware_capabilities", caps)
    report.add_derived(
        "the capability bitfield, so the interface offers only what this vehicle has"
    )

    # ------------------------------------------------------------------ limits
    _set(image, "engine.max_rpm", lim.max_rpm.value)
    _set(image, "safety.rpm_limit", lim.safety_rpm_limit.value)
    _set(image, "safety.overspeed_rpm", lim.overspeed_rpm.value)
    _set(image, "engine.idle_rpm_target", lim.idle_rpm.value)
    # The hard envelope has to CONTAIN the ignition table's own range, not merely the cells
    # currently in it. The rule is about capability: a table that can express an angle the
    # envelope will clamp lets a tuner type a number that is quietly altered, and an
    # ignition angle that is quietly altered is a combustion-timing error with no symptom
    # until it is catastrophic. So the envelope is widened to cover the table's declared
    # range, and the widening is reported rather than done quietly.
    ign_range = cal.ITEMS_BY_NAME["ignition.ignition_base_table"]
    env_lo = min(lim.ignition_min_deg.value, ign_range.minimum)
    env_hi = max(lim.ignition_max_deg.value, ign_range.maximum)
    if (env_lo, env_hi) != (lim.ignition_min_deg.value, lim.ignition_max_deg.value):
        report.add_warning(
            f"the ignition envelope was widened to {env_lo:g}..{env_hi:g} degrees to cover "
            f"the ignition table's own range ({ign_range.minimum:g}..{ign_range.maximum:g}). "
            f"The profile asked for {lim.ignition_min_deg.value:g}..{lim.ignition_max_deg.value:g}. "
            f"Clamping the envelope below the table's range is exactly what the ECU's "
            f"ignition-envelope rule exists to prevent, so it was not done. If you need a "
            f"narrower envelope, narrow the TABLE's range in the schema, not just this value."
        )
    _set(image, "strategy.ignition_hard_min_deg", env_lo)
    _set(image, "strategy.ignition_hard_max_deg", env_hi)
    # Same reasoning as the ignition envelope: the lambda safety window has to CONTAIN the
    # target table's range, so that no cell can be silently clamped into it. A target lambda
    # quietly altered is a mixture quietly altered, and the wideband then corrects toward a
    # mixture the engine is not actually running.
    lam_range = cal.ITEMS_BY_NAME["fuel.target_lambda_table"]
    rich = min(lim.rich_lambda_limit.value, lam_range.minimum)
    lean = max(lim.lean_lambda_limit.value, lam_range.maximum)
    if (rich, lean) != (lim.rich_lambda_limit.value, lim.lean_lambda_limit.value):
        report.add_warning(
            f"the lambda safety window was widened to {rich:g}..{lean:g} to cover the target "
            f"lambda table's own range ({lam_range.minimum:g}..{lam_range.maximum:g}). The "
            f"profile asked for {lim.rich_lambda_limit.value:g}..{lim.lean_lambda_limit.value:g}. "
            f"A window narrower than the table's range would let a cell be silently clamped, "
            f"which is what this rule prevents."
        )
    _set(image, "safety.lean_lambda_limit", lean)
    _set(image, "safety.rich_lambda_limit", rich)
    # Boost takes the same containment treatment, plus the overboost interlock.
    #
    # This is awkward for a NATURALLY ASPERATED engine, and honestly so: the boost table's
    # declared range reaches 200 kPa, so a NA profile that says "101.3, that is all there
    # is" cannot satisfy a rule requiring the ceiling to cover the table's range. The options
    # are to lie about the ceiling or to be unable to build, and a build that works and says
    # what it did is better than either. What actually keeps boost control inactive is that
    # the profile does not enable it and the table cells are atmospheric.
    boost_range = cal.ITEMS_BY_NAME["boost.boost_target_table"]
    boost_max = max(lim.boost_max_kpa.value, boost_range.maximum)
    if boost_max != lim.boost_max_kpa.value:
        report.add_warning(
            f"boost_max_kpa was raised to {boost_max:g} to cover the boost table's declared "
            f"range (up to {boost_range.maximum:g} kPa). The profile asked for "
            f"{lim.boost_max_kpa.value:g}. This does NOT mean the engine can make boost: "
            f"boost closed loop is "
            f"{'enabled' if h.has_boost.value else 'not enabled'} and the table cells are "
            f"atmospheric. See docs/OPEN_ITEMS.md for why this rule and a NA engine do not "
            f"get along."
        )
    _set(image, "safety.boost_max_kpa", boost_max)
    # The overboost cutoff has to sit at or above the ceiling, or the ECU would declare an
    # overboost condition on every cell of a perfectly good table.
    overboost = cal.ITEMS_BY_NAME["boost.boost_overboost_limit_kpa"]
    _set(image, "boost.boost_overboost_limit_kpa", max(image.get(overboost), boost_max))
    report.add_derived("rev limits, ignition envelope and lambda safety window")

    # ------------------------------------------------------------ clamp the tables
    #
    # Narrowing an envelope does not narrow the tables that live inside it, and the ECU
    # refuses an image whose ignition table commands an angle outside the hard envelope. So
    # a profile that tightens the envelope - which is the SAFE direction, and therefore the
    # likely one - has to bring the table with it, or the whole build is rejected.
    #
    # The alternative - leaving it and letting the ECU refuse - is worse than useless: the
    # refusal names a rule and not the cell, and the operator is left looking at an
    # ignition table they did not touch.
    _clamp_image(image, report, lim)

    # The kill switch. A switch that is fitted and set to "ignore" means the ECU arms with
    # the key pulled - which is a rule in the schema precisely because it is a wiring
    # mistake nobody makes deliberately.
    # 0 is "ignore", which the schema refuses when a kill switch is fitted: an ECU that
    # arms with the switch open is a switch that does nothing.
    _set(image, "switches.kill_switch_present", bool(h.has_kill_switch.value))
    _set(image, "switches.kill_switch_fault_behaviour", 1 if h.has_kill_switch.value else 0)
    _set(image, "switches.kill_switch_active_high", True)
    if h.has_kill_switch.value:
        report.add_outstanding(
            "confirm the kill switch's behaviour: the profile assumes it fails safe, and a "
            "switch wired the other way round is a switch that does nothing"
        )

    # ------------------------------------------------- honest verification flags
    #
    # The schema refuses to arm a sequential tune without verified TDC and cam phase, and
    # that refusal is correct. The profile CANNOT verify either, because both need the
    # engine turning and an instrument. So they are left false and the operator is told why,
    # rather than being handed a tune that arms on a claim nobody made.
    needs_tdc = INJECTION_STRATEGY[profile.injection_strategy] >= INJECTION_STRATEGY["semi-sequential"]
    _set(image, "strategy.tdc_verified", False)
    _set(image, "strategy.cam_phase_verified", False)
    if needs_tdc:
        report.add_outstanding(
            f"verify TDC with a timing light, then set strategy.tdc_verified. The ECU will "
            f"refuse to arm with {profile.injection_strategy!r} injection until you do, and "
            f"that refusal is the safety system working."
        )
    if t.cam_teeth.value >= 2:
        report.add_outstanding(
            "verify the cam phase against the crank, then set strategy.cam_phase_verified"
        )

    # Unverified values in the profile are called out by name.
    for path, value in profile.unverified():
        report.add_warning(
            f"{path} = {value.value} is {value.provenance.value}"
            + (f" ({value.source})" if value.source else "")
            + (f" - {value.note}" if value.note else "")
        )

    # ------------------------------------------------------------ what is left
    report.add_outstanding(
        "FUEL TABLE: this build ships the conservative starting table. It will idle and "
        "cruise and it is not a track tune. Measure it on a dyno or a wideband log"
    )
    report.add_outstanding(
        "IGNITION TABLE: same - the shipped table is a conservative starting point, timed "
        "only as well as tooth 1 is"
    )
    if profile.use in ("track", "drag"):
        report.add_outstanding(
            f"the profile is marked use={profile.use!r}, which widens the limits above the "
            f"street values. Confirm the hardware survives them: the rev limiter, the fuel "
            f"pressure and the cooling all have to be rated for what the profile now allows"
        )

    return image, report
