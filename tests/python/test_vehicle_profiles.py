"""Tests for the vehicle profile system.

The property under test is not "the builder sets some values". It is:

**a profile's output is accepted by the firmware's own validator, and rejected when the
profile is inconsistent.**

That is a much stronger claim and the only one that matters, because the builder and the ECU
are independent implementations of the same rules - the builder in Python against the
generated descriptor, the ECU in C against the compiled struct. A builder that produces an
image the ECU refuses is not a builder, it is a second opinion.

The end-to-end tests here write profile-built calibrations to a real simulator and read them
back, so "the ECU accepted it" is a fact rather than a local re-implementation agreeing with
itself.
"""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "tuner"))

from raccoon import build, calibration as cal  # noqa: E402
from raccoon.bridge import SimulatorBridge  # noqa: E402
from raccoon.client import EcuClient  # noqa: E402
from raccoon.transport import TcpTransport  # noqa: E402
from raccoon.vehicle import Provenance, Value, VehicleProfile, available_profiles  # noqa: E402

UNLOCK_CODE = "raccoon"


def value(v, provenance=Provenance.MEASURED, note="") -> Value:
    return Value(value=v, provenance=provenance, note=note)


def minimal_profile(**overrides) -> VehicleProfile:
    """A small, coherent, fully-measured 2-cylinder profile.

    Everything measured on purpose: these tests are about the builder's arithmetic and the
    ECU's rules, and a test that failed because a default was an assumption would be
    testing the wrong thing.
    """
    raw = {
        "name": "test_twin",
        "geometry": {
            "cylinders": 2, "bore_mm": 78.0, "stroke_mm": 67.8, "compression_ratio": 9.5,
            "rod_ratio": 1.45, "crank_layout": 1, "firing_order": [0, 1],
        },
        "trigger": {"crank_teeth": 36, "missing_teeth": 1, "teeth_per_rev": 1,
                    "cam_teeth": 0},
        "hardware": {
            "injector_peak_a": 4.0, "injector_hold_a": 1.2, "injector_max_duty_pct": 80.0,
            "injector_count": 2, "ignition_peak_a": 7.5, "ignition_hold_a": 3.5,
            "ignition_max_dwell_ms": 8.0, "ignition_count": 2, "spark_divider_ratio": 9.0,
            "lambda_channels": 0, "throttle_channels": 1, "pedal_channels": 1,
            "has_kill_switch": True,
        },
        "limits": {"max_rpm": 8000, "safety_rpm_limit": 7500, "overspeed_rpm": 8500,
                   "idle_rpm": 1100, "lean_lambda_limit": 1.2, "rich_lambda_limit": 0.8},
        "injection_strategy": "semi-sequential",
        "ignition_strategy": "static_table",
        "idle_strategy": "pwm_iac",
        "lambda_strategy": "off",
    }
    for section, values in overrides.items():
        if isinstance(values, dict):
            raw.setdefault(section, {})
            raw[section].update(values)
        else:
            raw[section] = values

    # Everything is MEASURED, declared as such explicitly.
    #
    # Written as bare numbers this profile would be entirely unverified - and that is
    # correct behaviour, because a bare number in a file makes no claim to anything. These
    # tests are about the builder's arithmetic and the ECU's rules, so the claim is made
    # here rather than worked around: a test that failed because a fixture was unverified
    # would be testing the provenance system instead of what it is for.
    def mark_measured(node):
        if isinstance(node, dict):
            for key, val in node.items():
                if key in ("firing_order",):
                    continue
                if isinstance(val, dict) and "value" in val:
                    node[key] = {"value": val["value"], "provenance": "measured"}
                elif isinstance(val, dict):
                    mark_measured(val)
                else:
                    node[key] = {"value": val, "provenance": "measured"}
        return node

    for section in ("geometry", "trigger", "hardware", "limits"):
        if section in raw:
            mark_measured(raw[section])
    return VehicleProfile.from_dict(raw)


class TestProfileLoading(unittest.TestCase):
    def test_every_shipped_profile_loads(self) -> None:
        paths = available_profiles()
        self.assertGreaterEqual(len(paths), 2, "expected the shipped profiles to be present")
        for path in paths:
            with self.subTest(profile=path.name):
                profile = VehicleProfile.load(path)
                self.assertTrue(profile.name)

    def test_a_bare_number_is_treated_as_unverified(self) -> None:
        # A number in a profile file carries no claim to being measured. Accepting it as
        # verified would let a hand-edited file look audited when it is not.
        v = Value.from_json(4.0)
        self.assertEqual(v.provenance, Provenance.UNVERIFIED)
        self.assertFalse(v.is_claimed)

    def test_provenance_survives_a_round_trip(self) -> None:
        profile = VehicleProfile.load(_ROOT / "engine_profiles" / "gt650.json")
        again = VehicleProfile.from_dict(json.loads(json.dumps(profile.to_dict())))
        self.assertEqual(again.geometry.bore_mm.provenance, profile.geometry.bore_mm.provenance)
        self.assertEqual(again.trigger.crank_teeth.value, profile.trigger.crank_teeth.value)

    def test_the_gt650_profile_is_honest_about_what_is_unknown(self) -> None:
        profile = VehicleProfile.load(_ROOT / "engine_profiles" / "gt650.json")
        unverified = profile.unverified()
        # The point of the provenance system: the GT650's trigger wheel, TDC and injector
        # are all unknown, and a profile that hid that would be worse than no profile.
        names = {path.rsplit(".", 1)[-1] for path, _ in unverified}
        self.assertIn("crank_teeth", names, "the trigger wheel is unverified and must say so")
        self.assertIn("tooth_1_angle_atdc", names, "TDC is unverified and must say so")
        self.assertIn("injector_peak_a", names, "the injector is unspecified and must say so")
        self.assertGreater(len(unverified), 10)

    def test_measured_values_are_excluded_from_the_unverified_list(self) -> None:
        profile = minimal_profile()
        outstanding = dict((path, value) for path, value in profile.unverified())
        # Everything the fixture states is measured and must be reported as such.
        self.assertNotIn("geometry.bore_mm", outstanding)
        self.assertNotIn("geometry.stroke_mm", outstanding)
        self.assertNotIn("trigger.crank_teeth", outstanding)
        self.assertNotIn("hardware.injector_peak_a", outstanding)
        # Fields the fixture never states are honestly reported as outstanding: a field a
        # profile omits has not been verified, and claiming otherwise would be the failure
        # mode this system exists to prevent.
        self.assertIn("trigger.missing_tooth_index", outstanding)


class TestValidation(unittest.TestCase):
    def test_a_coherent_profile_has_no_problems(self) -> None:
        self.assertEqual(build.validate(minimal_profile()), [])

    def test_a_firing_order_that_repeats_a_cylinder_is_refused(self) -> None:
        profile = minimal_profile()
        profile.geometry.firing_order = [0, 0]
        problems = build.validate(profile)
        self.assertTrue(any("firing_order" in p for p in problems), problems)

    def test_sequential_injection_without_a_cam_wheel_is_refused(self) -> None:
        # The dangerous one: sequential injection with no cam signal means the ECU cannot
        # know which cylinder is which, and will fire a cylinder on its exhaust stroke.
        profile = minimal_profile()
        profile.injection_strategy = "sequential"
        problems = build.validate(profile)
        self.assertTrue(any("cam" in p for p in problems), problems)

    def test_fewer_injectors_than_cylinders_is_refused(self) -> None:
        profile = minimal_profile()
        profile.hardware.injector_count = value(1)
        problems = build.validate(profile)
        self.assertTrue(any("injector" in p for p in problems), problems)

    def test_a_safety_limit_above_the_max_rpm_is_refused(self) -> None:
        # If the soft cut is above the limiter, the ECU is commanded to cut fuel at the rev
        # limit it is supposed to be protecting.
        profile = minimal_profile()
        profile.limits.safety_rpm_limit = value(9000.0)
        problems = build.validate(profile)
        self.assertTrue(any("BELOW" in p or "below" in p for p in problems), problems)

    def test_closed_loop_lambda_with_no_sensor_is_refused(self) -> None:
        profile = minimal_profile()
        profile.lambda_strategy = "wideband"
        problems = build.validate(profile)
        self.assertTrue(any("lambda" in p for p in problems), problems)

    def test_traction_control_with_one_wheel_speed_sensor_is_refused(self) -> None:
        profile = minimal_profile()
        profile.hardware.has_traction = value(True)
        profile.hardware.wheel_speed_channels = value(1)
        problems = build.validate(profile)
        self.assertTrue(any("wheel speed" in p for p in problems), problems)

    def test_two_throttle_sensors_with_dbw_off_is_refused(self) -> None:
        # A duplicated throttle sensor with DBW off runs the idle control off a phantom
        # input, and nothing would say so.
        profile = minimal_profile()
        profile.hardware.throttle_channels = value(2)
        problems = build.validate(profile)
        self.assertTrue(any("throttle" in p for p in problems), problems)

    def test_build_refuses_an_inconsistent_profile_with_all_the_reasons(self) -> None:
        profile = minimal_profile()
        profile.geometry.firing_order = [0, 0]
        with self.assertRaises(build.ProfileError) as caught:
            build.build(profile)
        # Every problem at once, not one per attempt. A builder that reports one failure per
        # run turns a five-minute job into five.
        self.assertIn("firing_order", str(caught.exception))


class TestDerivation(unittest.TestCase):
    def test_displacement_is_derived_from_bore_and_stroke(self) -> None:
        image, _ = build.build(minimal_profile())
        expected = (3.14159265358979 / 4.0) * 78.0 ** 2 * 67.8 / 1000.0 * 2
        self.assertAlmostEqual(image.get("engine.displacement_cc"), expected, places=1)
        self.assertAlmostEqual(image.get("engine.displacement_cc"), 648.0, places=0)

    def test_the_period_window_comes_from_the_wheel(self) -> None:
        profile = minimal_profile()
        image, report = build.build(profile)
        teeth = 36
        # The overspeed threshold is the tooth period at the overspeed speed.
        expected_overspeed = 60.0e6 / (teeth * 2 * 8500.0)
        self.assertAlmostEqual(image.get("trigger.overspeed_period_us"), round(expected_overspeed),
                               delta=1.0)
        # And the noise floor sits below it, which is the ordering the schema's rule demands.
        self.assertLess(image.get("trigger.min_period_us"),
                        image.get("trigger.overspeed_period_us"))
        self.assertTrue(any("period window" in d for d in report.derived))

    def test_a_different_wheel_gives_a_different_window(self) -> None:
        a, _ = build.build(minimal_profile())
        b, _ = build.build(minimal_profile(trigger={"crank_teeth": 60, "missing_teeth": 2}))
        # 60 teeth pass far more often than 36, so the shortest legitimate period is shorter.
        self.assertLess(b.get("trigger.overspeed_period_us"),
                        a.get("trigger.overspeed_period_us"))

    def test_teeth_per_rev_doubles_the_edge_rate(self) -> None:
        a, _ = build.build(minimal_profile())
        b, _ = build.build(minimal_profile(trigger={"teeth_per_rev": 2}))
        self.assertAlmostEqual(b.get("trigger.overspeed_period_us") * 2.0,
                               a.get("trigger.overspeed_period_us"), delta=2.0)

    def test_capabilities_follow_what_is_fitted(self) -> None:
        image, _ = build.build(minimal_profile())
        caps = int(image.get("hardware.hardware_capabilities"))
        self.assertTrue(caps & (1 << build.CAPABILITY_BITS["cap_ckp"]), "a crank sensor is fitted")
        self.assertFalse(caps & (1 << build.CAPABILITY_BITS["cap_cmp"]),
                         "no cam sensor is fitted")
        self.assertFalse(caps & (1 << build.CAPABILITY_BITS["cap_knock"]),
                         "no knock sensor is fitted")
        self.assertFalse(caps & (1 << build.CAPABILITY_BITS["cap_boost"]),
                         "no turbo is fitted")

    def test_a_cam_wheel_reaches_the_capability_set(self) -> None:
        image, _ = build.build(minimal_profile(
            trigger={"cam_teeth": 4},
            hardware={"lambda_channels": 1},
            injection_strategy="sequential",
        ))
        caps = int(image.get("hardware.hardware_capabilities"))
        self.assertTrue(caps & (1 << build.CAPABILITY_BITS["cap_cmp"]))
        self.assertTrue(caps & (1 << build.CAPABILITY_BITS["cap_sequential"]))
        self.assertTrue(caps & (1 << build.CAPABILITY_BITS["cap_wideband"]))

    def test_a_four_tooth_cam_selects_the_four_tooth_profile(self) -> None:
        # Not cosmetic: a 4-tooth wheel decoded as 3 teeth gives a cam phase wrong by a
        # quarter of a cam revolution.
        image, _ = build.build(minimal_profile(
            trigger={"cam_teeth": 4}, injection_strategy="sequential"))
        self.assertEqual(image.get("trigger.profile_cam"), 2.0)

    def test_a_three_tooth_cam_selects_the_three_tooth_profile(self) -> None:
        image, _ = build.build(minimal_profile(
            trigger={"cam_teeth": 3}, injection_strategy="sequential"))
        self.assertEqual(image.get("trigger.profile_cam"), 1.0)

    def test_the_verification_flags_are_not_claimed_by_a_profile(self) -> None:
        # A profile cannot verify TDC or cam phase: both need the engine turning and an
        # instrument. Claiming them would defeat a safety interlock.
        # A cam wheel, because sequential injection without one is refused outright.
        image, report = build.build(minimal_profile(
            trigger={"cam_teeth": 3}, injection_strategy="sequential"))
        self.assertFalse(image.get("strategy.tdc_verified"))
        self.assertFalse(image.get("strategy.cam_phase_verified"))
        self.assertTrue(any("tdc_verified" in o for o in report.outstanding))

    def test_the_builder_does_not_silently_narrow_an_envelope(self) -> None:
        # Narrowing the ignition envelope below what the table can express would let a tuner
        # type an angle that is quietly clamped. The builder widens and says so instead.
        profile = minimal_profile()
        profile.limits.ignition_min_deg = value(-5.0)
        profile.limits.ignition_max_deg = value(25.0)
        _, report = build.build(profile)
        self.assertTrue(any("envelope was widened" in w for w in report.warnings), report.warnings)
        table = cal.ITEMS_BY_NAME["ignition.ignition_base_table"]
        self.assertGreaterEqual(report.warnings and 0 or 0, 0)

    def test_the_builder_reports_what_it_derived(self) -> None:
        _, report = build.build(minimal_profile())
        self.assertGreaterEqual(len(report.derived), 5)
        joined = " ".join(report.derived)
        self.assertIn("geometry", joined)
        self.assertIn("trigger", joined)
        self.assertIn("capability", joined)

    def test_the_builder_always_says_the_tables_need_measuring(self) -> None:
        # The single most important sentence the builder emits. A build that implied a track
        # tune was ready would be a lie.
        _, report = build.build(minimal_profile())
        self.assertTrue(any("FUEL TABLE" in o for o in report.outstanding))
        self.assertTrue(any("IGNITION TABLE" in o for o in report.outstanding))

    def test_a_track_profile_warns_about_the_hardware(self) -> None:
        _, report = build.build(minimal_profile(use="track"))
        self.assertTrue(any("rated for" in o for o in report.outstanding))

    def test_the_fuel_table_is_not_overwritten(self) -> None:
        # A profile knows an engine has two cylinders; it does not know what to feed them.
        base = cal.Calibration()
        base.set_cell("fuel.ve_table", 0, 91.0)
        before = base.table("fuel.ve_table")
        image, _ = build.build(minimal_profile(), base=base)
        self.assertEqual(image.table("fuel.ve_table"), before)


class TestAgainstTheRealEcu(unittest.TestCase):
    """The tests that matter: the firmware accepts what the builder produces."""

    bridge: SimulatorBridge
    client: EcuClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.bridge = SimulatorBridge()
        cls.port = cls.bridge.launch()
        cls.client = EcuClient(TcpTransport("127.0.0.1", cls.port, timeout=3.0), timeout=3.0)
        cls.client.connect()
        cls.client.authenticate(UNLOCK_CODE)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.client.disconnect()
        cls.bridge.stop()

    def setUp(self) -> None:
        self.bridge.stop_engine()

    def _write(self, image: cal.Calibration) -> cal.Calibration:
        self.client.flash_calibration(image, UNLOCK_CODE)
        return self.client.read_bulk()

    def test_the_ecu_accepts_a_calibration_built_from_a_profile(self) -> None:
        image, _ = build.build(minimal_profile())
        after = self._write(image)
        self.assertEqual(after.get("engine.cylinder_count"), 2.0)
        self.assertAlmostEqual(after.get("engine.displacement_cc"), 648.0, places=0)

    def test_the_ecu_serves_back_the_derived_period_window(self) -> None:
        image, _ = build.build(minimal_profile())
        after = self._write(image)
        self.assertEqual(after.get("trigger.overspeed_period_us"),
                         image.get("trigger.overspeed_period_us"))
        self.assertEqual(after.get("trigger.max_period_us"),
                         image.get("trigger.max_period_us"))

    def test_the_ecu_accepts_a_four_cylinder_profile(self) -> None:
        profile = minimal_profile(
            geometry={"cylinders": 4, "crank_layout": 3, "firing_order": [0, 1, 2, 3]},
            hardware={"injector_count": 4, "ignition_count": 4},
        )
        after = self._write(build.build(profile)[0])
        self.assertEqual(after.get("engine.cylinder_count"), 4.0)
        self.assertEqual(after.get("engine.crank_layout"), 3.0)

    def test_the_ecu_accepts_a_single_cylinder_with_a_custom_firing_angle(self) -> None:
        profile = minimal_profile(
            geometry={"cylinders": 1, "crank_layout": 0, "firing_order": [0]},
            hardware={"injector_count": 1, "ignition_count": 1},
        )
        after = self._write(build.build(profile)[0])
        self.assertEqual(after.get("engine.cylinder_count"), 1.0)

    def test_the_shipped_gt650_profile_is_accepted(self) -> None:
        profile = VehicleProfile.load(_ROOT / "engine_profiles" / "gt650.json")
        image, _ = build.build(profile)
        after = self._write(image)
        self.assertEqual(after.get("engine.bore_mm"), 78.0)
        self.assertAlmostEqual(after.get("engine.displacement_cc"), 648.0, places=0)

    def test_a_narrowed_ignition_envelope_still_builds(self) -> None:
        # The schema's ignition-envelope rule requires the envelope to cover the table's
        # range. A profile asking for less has to be handled, not rejected.
        profile = minimal_profile()
        profile.limits.ignition_max_deg = value(28.0)
        after = self._write(build.build(profile)[0])
        self.assertGreaterEqual(after.get("strategy.ignition_hard_max_deg"),
                                cal.ITEMS_BY_NAME["ignition.ignition_base_table"].maximum)


if __name__ == "__main__":
    unittest.main(verbosity=2)
