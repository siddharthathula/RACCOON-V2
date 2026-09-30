"""A vehicle profile: the ECU's description of one particular engine.

The specification is explicit that the engine core must contain no application-specific
logic - no ``if GT650`` anywhere - and that all vehicle configuration is *data*. This module
is where that data lives, and :mod:`raccoon.vehicle` turns it into a calibration.

A profile is deliberately **not** a tune. It is the physical description of a machine:

* geometry - bore, stroke, compression ratio, firing order;
* the trigger wheel - teeth, missing teeth, where tooth 1 sits, cam tooth count;
* the hardware fitted - injector and coil current limits, maximum dwell, what is grounded;
* what is fitted and wired - wideband, knock, wheel speeds, DBW, boost, VVT;
* limits - the rev ceiling, the safety envelope.

Everything here is either a measurement, a datasheet value, or a wiring fact. Every field
carries provenance, because the single most dangerous thing about a tune is a number nobody
can account for. :class:`Provenance` is what makes the difference between "this came off the
injector datasheet" and "someone typed 4.0 once and it stayed".

The fuel and ignition *tables* are not here. A profile knows an engine has 4 cylinders; it
does not know what to feed them at 4000 rpm and 60 kPa. That is measured on a dyno, and
pretending otherwise is how engines are damaged.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from raccoon import calibration as cal

PROFILE_DIR = Path(__file__).resolve().parents[2] / "engine_profiles"


class Provenance(str, Enum):
    """Where a number came from. Carried all the way into the calibration reference.

    The important distinction is between MEASURED (a dial read it) and ASSUMED (somebody
    thought so). Both can be right; only one of them can be checked, and the tuner needs to
    tell them apart before committing a tune to a track.
    """

    MEASURED = "measured"          # a dial, a flow meter, a timing light
    DATASHEET = "datasheet"        # the part's published specification
    WIRING = "wiring"              # established by continuity or a wiring diagram
    ASSUMED = "assumed"            # a reasoned estimate, explicitly not verified
    UNVERIFIED = "unverified"      # a placeholder the owner must replace


# Values that may be ASSUMED or UNVERIFIED but must never be presented as verified.
CLAIMED = frozenset({Provenance.MEASURED, Provenance.DATASHEET, Provenance.WIRING})


@dataclass
class Value:
    """One number, with where it came from and a note saying why.

    A bare float in a configuration file is indistinguishable from a measurement, and that
    is precisely the problem: a tune built from a mixture of the two cannot be reasoned about
    when the engine misbehaves. Wrapping every value costs a little typing and makes the
    whole profile auditable.
    """

    value: float
    provenance: Provenance = Provenance.UNVERIFIED
    source: str = ""
    note: str = ""

    def __post_init__(self) -> None:
        if isinstance(self.provenance, str):
            self.provenance = Provenance(self.provenance)

    def to_raw(self) -> float:
        """The value as a number.

        Enum-valued items hold their NAME as a string so a profile file is readable, and the
        name is mapped to a number by the builder against the generated schema. A name that
        reaches this method has not been mapped, and returning the string's length would be
        the worst possible failure - a plausible number that is silently wrong. So it is a
        hard error.
        """
        if isinstance(self.value, str):
            raise TypeError(
                f"{self.value!r} is an enum name, not a number; it has to be mapped to its "
                f"schema value before use"
            )
        return float(self.value)

    @property
    def is_claimed(self) -> bool:
        return self.provenance in CLAIMED

    def number(self) -> float:
        """The numeric value, for the comparisons a profile is validated with."""
        if isinstance(self.value, str):
            return 0.0
        return float(self.value)

    def to_json(self) -> dict:
        return {
            "value": self.value,
            "provenance": self.provenance.value,
            "source": self.source,
            "note": self.note,
        }

    @classmethod
    def from_json(cls, raw: dict | int | float | str) -> "Value":
        """Accept a number, a string, or a full object.

        A string is allowed because several items are enums and a profile should be able to
        say {"value": "semi-sequential"} rather than an unexplained integer. The string is
        kept as-is; the enum maps live in :mod:`raccoon.build`, where the names can be
        checked against the schema.
        """
        if isinstance(raw, dict):
            value = raw.get("value", 0.0)
            if isinstance(value, str):
                # Kept as the string; to_raw() below is what turns it into a number.
                pass
            else:
                value = float(value)
            return cls(
                value=value,  # type: ignore[arg-type]
                provenance=Provenance(raw.get("provenance", "unverified")),
                source=raw.get("source", ""),
                note=raw.get("note", ""),
            )
        # A bare number in a profile file is accepted - profiles are edited by hand and a
        # number is much quicker to type - but it is treated as unverified, because a bare
        # number carries no claim to being anything else.
        if isinstance(raw, str):
            # An enum name written without provenance, which is how a hand-edited file looks
            # when someone only fills in the value. Kept as the name; to_raw() refuses to
            # treat it as a number.
            return cls(value=raw, provenance=Provenance.UNVERIFIED,
                       note="written as a bare enum name, so its provenance is unknown")
        return cls(value=float(raw), provenance=Provenance.UNVERIFIED,
                   note="written as a bare number, so its provenance is unknown")


@dataclass
class Trigger:
    """The crank and cam wheels as fitted."""

    crank_teeth: Value
    missing_teeth: Value = field(default_factory=lambda: Value(0))
    missing_tooth_index: Value = field(default_factory=lambda: Value(0))
    tooth_1_angle_atdc: Value = field(default_factory=lambda: Value(0.0))
    teeth_per_rev: Value = field(default_factory=lambda: Value(1))
    cam_teeth: Value = field(default_factory=lambda: Value(0))
    tooth_1_polarity: Value = field(default_factory=lambda: Value(1))
    input_filter_level: Value = field(default_factory=lambda: Value(1))
    sync_skip_revolutions: Value = field(default_factory=lambda: Value(0))

    def period_us(self, rpm: float) -> float:
        """Time between crank teeth at a given speed.

        Used to derive the decoder's period window. Getting this wrong is not a subtle
        failure: the window is what distinguishes a tooth from an overspeed, so a window set
        from the wrong tooth count either declares every tooth an overspeed at speed, or
        accepts a stall indefinitely.
        """
        teeth = max(1, int(round(self.crank_teeth.value * self.teeth_per_rev.value)))
        return 60.0e6 / (teeth * 2.0 * max(rpm, 1.0))   # four-stroke: two revolutions


@dataclass
class Hardware:
    """The output drivers, as actually wired."""

    injector_peak_a: Value = field(default_factory=lambda: Value(4.0))
    injector_hold_a: Value = field(default_factory=lambda: Value(1.2))
    injector_max_duty_pct: Value = field(default_factory=lambda: Value(85.0))
    injector_count: Value = field(default_factory=lambda: Value(2))
    ignition_peak_a: Value = field(default_factory=lambda: Value(7.5))
    ignition_hold_a: Value = field(default_factory=lambda: Value(3.5))
    ignition_max_dwell_ms: Value = field(default_factory=lambda: Value(8.0))
    ignition_count: Value = field(default_factory=lambda: Value(2))
    spark_divider_ratio: Value = field(default_factory=lambda: Value(9.0))
    knock_sensor_count: Value = field(default_factory=lambda: Value(0))
    wheel_speed_channels: Value = field(default_factory=lambda: Value(0))
    pedal_channels: Value = field(default_factory=lambda: Value(1))
    throttle_channels: Value = field(default_factory=lambda: Value(1))
    lambda_channels: Value = field(default_factory=lambda: Value(0))
    injector_driver_type: Value = field(default_factory=lambda: Value("high_z_saturated"))
    ignition_driver_topology: Value = field(default_factory=lambda: Value("smart_coil"))
    has_kill_switch: Value = field(default_factory=lambda: Value(True))
    has_boost: Value = field(default_factory=lambda: Value(False))
    has_vvt: Value = field(default_factory=lambda: Value(False))
    has_traction: Value = field(default_factory=lambda: Value(False))
    has_dbw: Value = field(default_factory=lambda: Value(False))


@dataclass
class Geometry:
    """The engine's dimensions."""

    cylinders: Value
    bore_mm: Value
    stroke_mm: Value
    compression_ratio: Value = field(default_factory=lambda: Value(9.5))
    rod_ratio: Value = field(default_factory=lambda: Value(1.45))
    crank_layout: Value = field(default_factory=lambda: Value(0))
    firing_order: list[int] = field(default_factory=lambda: [0, 1])

    def displacement_cc(self) -> float:
        """Single-cylinder displacement, from the standard piston area.

        This is the *derived* figure and the engine builder uses the same formula, so a
        profile that states a displacement and states a bore and stroke that disagree is
        caught at build time rather than quietly producing the wrong charge mass.
        """
        return (math.pi / 4.0) * (self.bore_mm.value ** 2) * self.stroke_mm.value / 1000.0


@dataclass
class Limits:
    """The envelope. Chosen wide enough to be safe and narrow enough to be a limit."""

    max_rpm: Value = field(default_factory=lambda: Value(9000.0))
    safety_rpm_limit: Value = field(default_factory=lambda: Value(8200.0))
    overspeed_rpm: Value = field(default_factory=lambda: Value(9500.0))
    idle_rpm: Value = field(default_factory=lambda: Value(1200.0))
    ignition_min_deg: Value = field(default_factory=lambda: Value(-10.0))
    ignition_max_deg: Value = field(default_factory=lambda: Value(35.0))
    lean_lambda_limit: Value = field(default_factory=lambda: Value(1.25))
    rich_lambda_limit: Value = field(default_factory=lambda: Value(0.75))
    boost_max_kpa: Value = field(default_factory=lambda: Value(101.3))


@dataclass
class VehicleProfile:
    """One engine, described completely enough to build a calibration from."""

    # The four sections are required and come first, because a profile without them is not a
    # profile. Everything after them has a default, so a hand-written JSON file can supply
    # only the parts it knows and still load.
    geometry: Geometry
    trigger: Trigger
    hardware: Hardware
    limits: Limits
    name: str = "unnamed"
    description: str = ""
    injection_strategy: str = "semi-sequential"
    ignition_strategy: str = "wasted_spark"
    idle_strategy: str = "pwm_iac"
    lambda_strategy: str = "off"
    use: str = "road"          # road | track | drag

    # ---------------------------------------------------------------- loading

    @classmethod
    def load(cls, path: Path | str) -> "VehicleProfile":
        raw = json.loads(Path(path).read_text())
        return cls.from_dict(raw)

    @classmethod
    def from_dict(cls, raw: dict) -> "VehicleProfile":
        def section(name: str) -> dict:
            return raw.get(name, {})

        geo = section("geometry")
        geometry = Geometry(
            cylinders=Value.from_json(geo.get("cylinders", 2)),
            bore_mm=Value.from_json(geo.get("bore_mm", 78.0)),
            stroke_mm=Value.from_json(geo.get("stroke_mm", 67.8)),
            compression_ratio=Value.from_json(geo.get("compression_ratio", 9.5)),
            rod_ratio=Value.from_json(geo.get("rod_ratio", 1.45)),
            crank_layout=Value.from_json(geo.get("crank_layout", 0)),
            firing_order=list(geo.get("firing_order", [0, 1])),
        )
        trg = section("trigger")
        trigger = Trigger(
            crank_teeth=Value.from_json(trg.get("crank_teeth", 36)),
            missing_teeth=Value.from_json(trg.get("missing_teeth", 0)),
            missing_tooth_index=Value.from_json(trg.get("missing_tooth_index", 0)),
            tooth_1_angle_atdc=Value.from_json(trg.get("tooth_1_angle_atdc", 0.0)),
            teeth_per_rev=Value.from_json(trg.get("teeth_per_rev", 1)),
            cam_teeth=Value.from_json(trg.get("cam_teeth", 0)),
            tooth_1_polarity=Value.from_json(trg.get("tooth_1_polarity", 1)),
            input_filter_level=Value.from_json(trg.get("input_filter_level", 1)),
            sync_skip_revolutions=Value.from_json(trg.get("sync_skip_revolutions", 0)),
        )
        hw = section("hardware")
        defaults = {
            "injector_peak_a": 4.0, "injector_hold_a": 1.2, "injector_max_duty_pct": 85.0,
            "injector_count": 2, "ignition_peak_a": 7.5, "ignition_hold_a": 3.5,
            "ignition_max_dwell_ms": 8.0, "ignition_count": 2, "spark_divider_ratio": 9.0,
            "knock_sensor_count": 0, "wheel_speed_channels": 0, "pedal_channels": 1,
            "throttle_channels": 1, "lambda_channels": 0, "has_kill_switch": True,
            "has_boost": False, "has_vvt": False, "has_traction": False, "has_dbw": False,
            "injector_driver_type": "high_z_saturated", "ignition_driver_topology": "smart_coil",
        }
        # Built field by field rather than with a dict comprehension over the defaults,
        # because the three kinds of value here need different handling: booleans stay
        # booleans, enum names stay strings, and numbers become numbers. A comprehension
        # that coerced them all through bool() turned every absent enum into True, which
        # then failed validation with a message pointing at the profile rather than at the
        # loader.
        bool_keys = ("has_kill_switch", "has_boost", "has_vvt", "has_traction", "has_dbw")
        hardware = Hardware()
        for key, default in defaults.items():
            if key in bool_keys:
                # Through from_json, not bool(). A profile file writes a flag as
                # {"value": false, "provenance": ...} so it can carry provenance like every
                # other value, and bool() of that dict is True - so has_traction became True
                # on a bike with no traction control, and validation then complained about
                # wheel speed sensors that were not fitted.
                setattr(hardware, key, Value.from_json(hw[key] if key in hw else default))
                got = getattr(hardware, key)
                if isinstance(got.value, bool):
                    got.value = bool(got.value)
            elif key in hw:
                setattr(hardware, key, Value.from_json(hw[key]))
            else:
                setattr(hardware, key, Value.from_json(default))
        lim = section("limits")
        limit_defaults = {
            "max_rpm": 9000.0, "safety_rpm_limit": 8200.0, "overspeed_rpm": 9500.0,
            "idle_rpm": 1200.0, "ignition_min_deg": -10.0, "ignition_max_deg": 35.0,
            "lean_lambda_limit": 1.25, "rich_lambda_limit": 0.75, "boost_max_kpa": 101.3,
        }
        limits = Limits(**{k: Value.from_json(lim[k]) for k in limit_defaults if k in lim})
        for key, default in limit_defaults.items():
            if not hasattr(limits, key):
                setattr(limits, key, Value(default))

        return cls(
            name=raw.get("name", "unnamed"),
            description=raw.get("description", ""),
            geometry=geometry,
            trigger=trigger,
            hardware=hardware,
            limits=limits,
            injection_strategy=raw.get("injection_strategy", "semi-sequential"),
            ignition_strategy=raw.get("ignition_strategy", "wasted_spark"),
            idle_strategy=raw.get("idle_strategy", "pwm_iac"),
            lambda_strategy=raw.get("lambda_strategy", "off"),
            use=raw.get("use", "road"),
        )

    def to_dict(self) -> dict:
        def dump(value: Value) -> dict:
            return value.to_json()

        return {
            "name": self.name,
            "description": self.description,
            "use": self.use,
            "injection_strategy": self.injection_strategy,
            "ignition_strategy": self.ignition_strategy,
            "idle_strategy": self.idle_strategy,
            "lambda_strategy": self.lambda_strategy,
            "geometry": {
                "cylinders": dump(self.geometry.cylinders),
                "bore_mm": dump(self.geometry.bore_mm),
                "stroke_mm": dump(self.geometry.stroke_mm),
                "compression_ratio": dump(self.geometry.compression_ratio),
                "rod_ratio": dump(self.geometry.rod_ratio),
                "crank_layout": dump(self.geometry.crank_layout),
                "firing_order": self.geometry.firing_order,
            },
            "trigger": {k: dump(v) for k, v in vars(self.trigger).items()},
            "hardware": {k: dump(v) for k, v in vars(self.hardware).items()},
            "limits": {k: dump(v) for k, v in vars(self.limits).items()},
        }

    # ------------------------------------------------------------------ audit

    def unverified(self) -> list[tuple[str, Value]]:
        """Every value that is not measured, from a datasheet, or established by wiring.

        The list is the point of the provenance system. An owner who wants to run this on a
        track needs to know exactly which numbers to check first, and "the ones in red" is a
        better answer than a general feeling that the file is untested.
        """
        out: list[tuple[str, Value]] = []

        def walk(prefix: str, obj) -> None:
            for name, value in vars(obj).items():
                if isinstance(value, Value):
                    if not value.is_claimed:
                        out.append((f"{prefix}.{name}", value))

        walk("geometry", self.geometry)
        walk("trigger", self.trigger)
        walk("hardware", self.hardware)
        walk("limits", self.limits)
        return out

    def summary(self) -> str:
        g = self.geometry
        return (
            f"{self.name}: {int(g.cylinders.number())} cylinder(s), "
            f"{g.bore_mm.number():g} x {g.stroke_mm.number():g} mm "
            f"= {g.displacement_cc() * g.cylinders.number():.0f} cc, CR {g.compression_ratio.number():g}:1, "
            f"{int(self.trigger.crank_teeth.number())} tooth crank"
            + (f" + {int(self.trigger.cam_teeth.number())} tooth cam" if self.trigger.cam_teeth.number() else "")
            + f", use={self.use}"
        )


def available_profiles() -> list[Path]:
    """Every profile on disk, sorted by name."""
    if not PROFILE_DIR.is_dir():
        return []
    return sorted(p for p in PROFILE_DIR.glob("*.json"))
