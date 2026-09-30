"""Cross-item consistency rules for the calibration schema.

``rc_schema`` validates each item in isolation.  That is necessary but not sufficient:
the most dangerous calibration mistakes are *relationships* between two items that are
each individually in range.  ``CALIBRATION_SPEC.md`` section 6 lists these rules in prose;
here they are machine-checked, so a mistake is a build failure and cannot reach a bike.

Two tiers exist on purpose:

``RULES``
    Build-time checks against the shipped defaults - the state the product ships in.

``firmware/src/calibration/cal_validate.c``
    Runtime checks against the *active* image.  A tuner can write any combination, so the
    ECU must re-check every relationship on activation; the ECU is authoritative.  Each
    rule name here is mirrored in ``rc_rules.h`` so the two tiers cannot drift, and
    ``tests/unit/test_rules.c`` asserts the lists agree.
"""

from __future__ import annotations

from typing import Callable, NamedTuple

from rc_schema import Item, Schema

Problem = str | None
Check = Callable[[Schema], Problem]


class Rule(NamedTuple):
    name: str
    text: str
    check: Check


def _eng(schema: Schema, name: str) -> float:
    """Engineering value of a scalar item's default."""
    item = schema.items[name]
    return float(item.to_engineering(item.raw_default))


def _enum(schema: Schema, name: str) -> str:
    item = schema.items[name]
    return item.enum_name(int(item.raw_default))


def rpm_limit_below_max_rpm(schema: Schema) -> Problem:
    limit, design = _eng(schema, "rpm_limit"), _eng(schema, "max_rpm")
    if limit >= design:
        return f"rpm_limit ({limit:.0f}) must be below max_rpm ({design:.0f})"
    return None


def overspeed_above_rpm_limit(schema: Schema) -> Problem:
    overspeed, limit = _eng(schema, "overspeed_rpm"), _eng(schema, "rpm_limit")
    if overspeed <= limit:
        return f"overspeed_rpm ({overspeed:.0f}) must be above rpm_limit ({limit:.0f})"
    return None


def idle_below_rpm_limit(schema: Schema) -> Problem:
    idle, limit = _eng(schema, "idle_rpm_target"), _eng(schema, "rpm_limit")
    if idle >= limit * 0.9:
        return (
            f"idle_rpm_target ({idle:.0f}) must sit well below rpm_limit ({limit:.0f}); "
            "a near-limit idle target makes the limiter chatter"
        )
    return None


def dwell_max_within_hardware(schema: Schema) -> Problem:
    soft, hard = _eng(schema, "dwell_max_ms"), _eng(schema, "ignition_max_dwell_ms")
    if soft > hard:
        return (
            f"dwell_max_ms ({soft:.2f}) must not exceed the hardware ceiling "
            f"ignition_max_dwell_ms ({hard:.2f})"
        )
    return None


def dwell_table_within_hardware_ceiling(schema: Schema) -> Problem:
    """A dwell cell beyond the hardware ceiling would be silently truncated by the
    hardware max-dwell timer, dumping the remaining coil energy in the IGBT. So the table
    may not be able to ask for more than the hardware will give."""
    table = schema.items["dwell_table"]
    hardware = _eng(schema, "ignition_max_dwell_ms")
    if table.cell_max > hardware:
        return (
            f"dwell_table can command {table.cell_max:g} ms, beyond the hardware ceiling "
            f"ignition_max_dwell_ms ({hardware:g}) ms; those cells would be truncated"
        )
    return None


def dwell_soft_limits_ordered(schema: Schema) -> Problem:
    lo, hi = _eng(schema, "dwell_min_ms"), _eng(schema, "dwell_max_ms")
    if lo >= hi:
        return f"dwell_min_ms ({lo:g}) must be below dwell_max_ms ({hi:g})"
    return None


def ignition_table_within_envelope(schema: Schema) -> Problem:
    table = schema.items["ignition_base_table"]
    lo, hi = _eng(schema, "ignition_hard_min_deg"), _eng(schema, "ignition_hard_max_deg")
    if table.cell_min < lo or table.cell_max > hi:
        return (
            f"ignition_base_table cell range [{table.cell_min:g},{table.cell_max:g}] deg "
            f"escapes the hard ignition envelope [{lo:g},{hi:g}] deg"
        )
    return None


def lambda_table_within_safety(schema: Schema) -> Problem:
    table = schema.items["target_lambda_table"]
    lean, rich = _eng(schema, "lean_lambda_limit"), _eng(schema, "rich_lambda_limit")
    if table.cell_min < rich:
        return (
            f"target_lambda_table can command {table.cell_min:g} lambda, richer than "
            f"rich_lambda_limit ({rich:g}); the safety limit would clamp it"
        )
    if table.cell_max > lean:
        return (
            f"target_lambda_table can command {table.cell_max:g} lambda, leaner than "
            f"lean_lambda_limit ({lean:g}); the safety limit would clamp it"
        )
    return None


def boost_table_within_limit(schema: Schema) -> Problem:
    table = schema.items["boost_target_table"]
    design_max = _eng(schema, "boost_max_kpa")
    overboost = _eng(schema, "boost_overboost_limit_kpa")
    if table.cell_max > design_max:
        return (
            f"boost_target_table can command {table.cell_max:g} kPa, above the design maximum "
            f"boost_max_kpa ({design_max:g}) kPa; the target would be refused at validation"
        )
    if overboost < design_max:
        return (
            f"boost_overboost_limit_kpa ({overboost:g}) is below boost_max_kpa ({design_max:g}); "
            "the emergency trip must sit above the design maximum, or boost could never reach it"
        )
    return None


def vvt_table_bounded(schema: Schema) -> Problem:
    """The VVT table holds a target cam angle; the actuator bound is a duty, so the
    meaningful check is that the target angle is within physical cam travel."""
    table = schema.items["vvt_target_table"]
    if abs(table.cell_min) > 60.0 or abs(table.cell_max) > 60.0:
        return (
            f"vvt_target_table spans [{table.cell_min:g},{table.cell_max:g}] deg; more than "
            "60 degrees of cam travel is not physically available on a street engine"
        )
    return None


def vvt_duty_limits_ordered(schema: Schema) -> Problem:
    lo, hi = _eng(schema, "vvt_min_duty_pct"), _eng(schema, "vvt_max_duty_pct")
    if lo >= hi:
        return f"vvt_min_duty_pct ({lo:g}) must be below vvt_max_duty_pct ({hi:g})"
    if lo < 0.0 or hi > 100.0:
        return f"vvt duty limits [{lo:g},{hi:g}] must lie inside [0,100] %"
    return None


def injector_duty_headroom(schema: Schema) -> Problem:
    limit = _eng(schema, "injector_duty_max_pct")
    if limit >= 100.0:
        return f"injector_duty_max_percent ({limit:g}) must leave headroom below 100 %"
    return None


def sequential_requires_cam(schema: Schema) -> Problem:
    if _enum(schema, "profile_cam") == "none" and _enum(schema, "injection_strategy") == "sequential":
        return (
            "injection_strategy defaults to sequential while profile_cam is 'none'; the ECU "
            "would refuse to arm. Ship a batch or semi-sequential default, or configure a cam."
        )
    return None


def firing_order_is_permutation(schema: Schema) -> Problem:
    order = sorted(int(schema.items[f"firing_order_cyl{i}"].raw_default) for i in range(4))
    if order != [0, 1, 2, 3]:
        return f"firing_order_cyl0..3 default to {order}, which is not a permutation of 0..3"
    return None


def min_period_below_max(schema: Schema) -> Problem:
    lo, hi = _eng(schema, "min_period_us"), _eng(schema, "max_period_us")
    if lo >= hi:
        return f"min_period_us ({lo:.0f}) must be below max_period_us ({hi:.0f})"
    return None


def overspeed_period_above_min(schema: Schema) -> Problem:
    overspeed, lo = _eng(schema, "overspeed_period_us"), _eng(schema, "min_period_us")
    if overspeed <= lo:
        return (
            f"overspeed_period_us ({overspeed:.0f}) must be above min_period_us ({lo:.0f}); "
            "otherwise every legal edge would be declared an overspeed fault"
        )
    return None


def knock_requires_sensor(schema: Schema) -> Problem:
    channels = int(schema.items["knock_sensor_count"].raw_default)
    if int(schema.items["knock_enabled"].raw_default) and channels == 0:
        return "knock_enabled is set while knock_sensor_count is 0: claiming knock control with no sensor"
    return None


def lambda_strategy_requires_sensor(schema: Schema) -> Problem:
    strategy = _enum(schema, "lambda_strategy")
    channels = int(schema.items["lambda_channel_count"].raw_default)
    if strategy == "narrowband" and channels == 0:
        return "lambda_strategy is narrowband while lambda_channel_count is 0"
    if strategy == "wideband_native_cj125" and channels == 0:
        return "lambda_strategy is wideband_native_cj125 while lambda_channel_count is 0"
    return None


def traction_requires_wheel_speed(schema: Schema) -> Problem:
    if int(schema.items["traction_enabled"].raw_default) and int(
        schema.items["wheel_speed_channel_count"].raw_default
    ) < 2:
        return (
            "traction_enabled is set while fewer than two wheel speed channels exist; "
            "slip cannot be computed without both axles"
        )
    return None


def dbw_requires_dual_channel(schema: Schema) -> Problem:
    if _enum(schema, "dbw_strategy") != "off":
        if int(schema.items["pedal_channel_count"].raw_default) < 2:
            return "dbw_strategy is not 'off' while pedal_channel_count < 2: DBW needs dual-channel redundancy"
        if int(schema.items["throttle_channel_count"].raw_default) < 2:
            return "dbw_strategy is not 'off' while throttle_channel_count < 2: DBW needs dual throttle"
    return None


def sequential_requires_verified_tdc(schema: Schema) -> Problem:
    if _enum(schema, "injection_strategy") in ("sequential", "staged") and not int(
        schema.items["tdc_verified"].raw_default
    ):
        return (
            "injection_strategy demands sequential operation but tdc_verified is false; the "
            "ECU would refuse to arm. Either verify TDC on the bench or ship a batch default"
        )
    return None


def sequential_requires_verified_cam(schema: Schema) -> Problem:
    if _enum(schema, "injection_strategy") in ("sequential", "staged") and not int(
        schema.items["cam_phase_verified"].raw_default
    ):
        return "sequential injection is selected but cam_phase_verified is false"
    return None


def throttle_dual_needs_dbw(schema: Schema) -> Problem:
    if int(schema.items["throttle_channel_count"].raw_default) >= 2 and _enum(schema, "dbw_strategy") == "off":
        return (
            "throttle_channel_count is 2 while dbw_strategy is 'off'; a cable throttle does "
            "not have two sensors, so this is either a wiring mistake or a mislabelled DBW"
        )
    return None


def kill_switch_blocks_arm(schema: Schema) -> Problem:
    if int(schema.items["kill_switch_present"].raw_default) and _enum(
        schema, "kill_switch_fault_behaviour"
    ) == "ignore":
        return (
            "kill_switch_fault_behaviour is 'ignore' while a kill switch is fitted: the arming "
            "gate would allow engine control with a broken kill switch"
        )
    return None


def modes_bitfield_covers_all(schema: Schema) -> Problem:
    """The ten named modes of spec section 48 must all fit in the mode bitfield."""
    item = schema.items["modes_enabled"]
    if len(item.bits) < 10:
        return (
            f"modes_enabled declares {len(item.bits)} bits but spec section 48 defines 10 modes "
            "(STREET SPORT TRACK DRAG QUALIFY RACE DYNO CUSTOM 1..3)"
        )
    if _eng(schema, "mode_count") > float(len(item.bits)):
        return f"mode_count ({_eng(schema, 'mode_count'):.0f}) exceeds the {len(item.bits)} available mode bits"
    return None


def mode_switch_coast_bounded(schema: Schema) -> Problem:
    coast = _eng(schema, "mode_switch_coast_ms")
    if not 0.0 <= coast <= 10000.0:
        return f"mode_switch_coast_ms ({coast:g}) must lie in [0,10000] ms"
    return None


def unverified_safety_limits_are_flagged(schema: Schema) -> Problem:
    """Safety items may ship as honest placeholders, but never as silent ones."""
    offenders: list[str] = []
    for item in schema.items.values():
        if item.owner == "SAFETY" and item.default_source == "UNVERIFIED" and item.source_ref:
            offenders.append(item.name)
    if offenders:
        return (
            "safety items should not carry a source_ref while still marked UNVERIFIED: "
            + ", ".join(sorted(offenders)[:6])
        )
    return None


def axis_breakpoints_increase(schema: Schema) -> Problem:
    """Every shipped axis must be strictly increasing.

    Checked at build time because the generator lays the axes out evenly, and an even layout
    is strictly increasing by construction - so this is really a proof that the generator
    did what it says, and that no axis was declared with a range too narrow to hold its own
    breakpoint count. An axis declared 0..3 with four breakpoints rounds four distinct
    values into two, and the table below it becomes unsearchable on a freshly flashed ECU.
    """
    for item in schema.items.values():
        if not item.is_axis:
            continue
        points = item.default_points()
        for a, b in zip(points, points[1:]):
            if not b > a:
                return f"{item.group}.{item.name} is not increasing at {a} -> {b}"
    return None


RULES: list[Rule] = [
    Rule("rpm_limit_below_max_rpm", "rpm_limit < max_rpm", rpm_limit_below_max_rpm),
    Rule("overspeed_above_rpm_limit", "overspeed_rpm > rpm_limit", overspeed_above_rpm_limit),
    Rule("idle_below_rpm_limit", "idle_rpm_target < 0.9 * rpm_limit", idle_below_rpm_limit),
    Rule("dwell_max_within_hardware", "dwell_max_ms <= ignition_max_dwell_ms", dwell_max_within_hardware),
    Rule("dwell_table_within_hardware_ceiling", "dwell_table.cell_max <= ignition_max_dwell_ms", dwell_table_within_hardware_ceiling),
    Rule("dwell_soft_limits_ordered", "dwell_min_ms < dwell_max_ms", dwell_soft_limits_ordered),
    Rule("ignition_table_within_envelope", "ignition_base_table inside the hard ignition envelope", ignition_table_within_envelope),
    Rule("lambda_table_within_safety", "target_lambda_table inside [rich_lambda_limit, lean_lambda_limit]", lambda_table_within_safety),
    Rule("boost_table_within_limit", "boost_target_table cells <= boost_max_kpa", boost_table_within_limit),
    Rule("vvt_table_bounded", "|vvt_target_table cells| <= 60 deg of cam travel", vvt_table_bounded),
    Rule("vvt_duty_limits_ordered", "0 <= vvt_min_duty_pct < vvt_max_duty_pct <= 100", vvt_duty_limits_ordered),
    Rule("injector_duty_headroom", "injector_duty_max_percent < 100", injector_duty_headroom),
    Rule("sequential_requires_cam", "sequential injection requires a cam source", sequential_requires_cam),
    Rule("firing_order_permutation", "firing_order_cyl0..3 form a permutation of 0..3", firing_order_is_permutation),
    Rule("min_period_below_max_period", "min_period_us < max_period_us", min_period_below_max),
    Rule("overspeed_period_above_min", "overspeed_period_us > min_period_us", overspeed_period_above_min),
    Rule("knock_requires_sensor", "knock_enabled implies knock_channel_count > 0", knock_requires_sensor),
    Rule("lambda_strategy_requires_sensor", "lambda_strategy implies a fitted channel", lambda_strategy_requires_sensor),
    Rule("traction_requires_wheel_speed", "traction_enabled implies 2 wheel speed channels", traction_requires_wheel_speed),
    Rule("dbw_requires_dual_channel", "dbw_strategy != off implies 2 pedal and 2 throttle channels", dbw_requires_dual_channel),
    Rule("sequential_requires_verified_tdc", "sequential injection implies tdc_verified", sequential_requires_verified_tdc),
    Rule("sequential_requires_verified_cam", "sequential injection implies cam_phase_verified", sequential_requires_verified_cam),
    Rule("throttle_dual_needs_dbw", "throttle_channel_count < 2 unless dbw_strategy != off", throttle_dual_needs_dbw),
    Rule("kill_switch_blocks_arm", "kill_switch_fault_action != ignore", kill_switch_blocks_arm),
    Rule("modes_bitfield_covers_all", "modes_enabled declares at least 10 bits for the 10 spec modes", modes_bitfield_covers_all),
    Rule("mode_switch_coast_bounded", "mode_switch_coast_ms in [0,10000]", mode_switch_coast_bounded),
    Rule("unverified_safety_not_sourced", "UNVERIFIED safety items carry no source_ref", unverified_safety_limits_are_flagged),
    Rule("axis_breakpoints_increase",
         "axis breakpoints must strictly increase; a repeated or inverted pair makes the "
         "table it belongs to unsearchable, so the lookup returns a value from whichever "
         "cell it happened to land in",
         axis_breakpoints_increase),
]


def validate_rules(schema: Schema) -> list[str]:
    """Run every cross-item rule and return the failures."""
    failures: list[str] = []
    for rule in RULES:
        problem = rule.check(schema)
        if problem:
            failures.append(f"{rule.name}: {problem}")
    return failures
