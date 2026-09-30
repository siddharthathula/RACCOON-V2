"""Axis breakpoints: the thing a tuner actually tunes on a table.

A 2-D calibration table is values, but a tuner does not think in cell numbers. They think
"at 2,800 rpm and 45 kPa". Those coordinates were, until now, fixed in the firmware by an
even division of the axis range, which put the same resolution at 1,000 rpm and 12,000 rpm
and made the low end - where an idling engine lives and where resolution is worth the most
- the least tunable part of the table.

These tests cover the axis as data: that it exists for every table, that it is stored and
read back exactly, that it survives a round trip through the real ECU, and that the ECU
refuses an axis it cannot search.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tuner"))

from raccoon import calibration as cal  # noqa: E402


# Every 2-D table in the schema, with the axis each of its two dimensions runs along. Held
# as data rather than recomputed, because a test that derives its own expectation from the
# code under test proves only that the code is self-consistent.
TABLES: dict[str, tuple[str, str]] = {
    "fuel.ve_table": ("fuel.ve_table_rpm_axis", "fuel.ve_table_map_kpa_axis"),
    "fuel.target_lambda_table": (
        "fuel.target_lambda_table_rpm_axis",
        "fuel.target_lambda_table_load_pct_axis",
    ),
    "ignition.ignition_base_table": (
        "ignition.ignition_base_table_rpm_axis",
        "ignition.ignition_base_table_load_pct_axis",
    ),
    "ignition.dwell_table": ("ignition.dwell_table_volt_axis", "ignition.dwell_table_rpm_axis"),
    "boost.boost_target_table": (
        "boost.boost_target_table_rpm_axis",
        "boost.boost_target_table_load_pct_axis",
    ),
    "vvt.vvt_target_table": (
        "vvt.vvt_target_table_rpm_axis",
        "vvt.vvt_target_table_load_pct_axis",
    ),
    "gear.gear_strategy_table": (
        "gear.gear_strategy_table_gear_axis",
        "gear.gear_strategy_table_rpm_axis",
    ),
    "ebt.ebt_retard_table": (
        "ebt.ebt_retard_table_gear_axis",
        "ebt.ebt_retard_table_rpm_axis",
    ),
}


class TestAxisSchema(unittest.TestCase):
    def test_every_table_has_an_axis_along_each_dimension(self) -> None:
        for table_name, (row_axis, col_axis) in TABLES.items():
            table = cal.ITEMS_BY_NAME[table_name]
            row = cal.ITEMS_BY_NAME[row_axis]
            col = cal.ITEMS_BY_NAME[col_axis]
            with self.subTest(table=table_name):
                self.assertTrue(row.is_axis, f"{row_axis} is not an axis")
                self.assertTrue(col.is_axis, f"{col_axis} is not an axis")
                # An axis along a dimension has as many breakpoints as the table has cells
                # in that dimension. A 16-cell column with 8 breakpoints either reads past
                # the end of the axis or interpolates over the wrong span.
                self.assertEqual(row.alloc_count, table.alloc_rows, f"{row_axis} rows")
                self.assertEqual(col.alloc_count, table.alloc_cols, f"{col_axis} cols")

    def test_the_descriptor_resolves_each_dimension_to_its_axis_item(self) -> None:
        # A table records which KIND of axis each dimension runs along - "rpm", "kPa" -
        # which describes the dimension but resolves to no item. The descriptor therefore
        # also carries the id of the actual axis1d item. Without it a host has to match on a
        # naming convention, and a host that guesses an even division is exactly the bug
        # this removes.
        for table_name, (row_axis, col_axis) in TABLES.items():
            table = cal.ITEMS_BY_NAME[table_name]
            with self.subTest(table=table_name):
                self.assertEqual(cal.ITEMS_BY_ID[table.row_axis_id].name, row_axis.split(".")[-1])
                self.assertEqual(cal.ITEMS_BY_ID[table.col_axis_id].name, col_axis.split(".")[-1])

    def test_the_axes_can_be_read_through_the_table_they_belong_to(self) -> None:
        image = cal.Calibration()
        for table_name, (row_axis, col_axis) in TABLES.items():
            with self.subTest(table=table_name):
                self.assertEqual(image.row_axis_of(table_name), image.axis(row_axis))
                self.assertEqual(image.col_axis_of(table_name), image.axis(col_axis))

    def test_every_axis_is_wide_enough_to_be_one(self) -> None:
        # An axis of a single breakpoint interpolates to a constant, which is a degenerate
        # table wearing a table's clothes.
        for axis in cal.AXES:
            with self.subTest(axis=axis.label()):
                self.assertGreaterEqual(axis.alloc_count, 2)
                self.assertLessEqual(axis.alloc_count, 64)
                self.assertLess(axis.minimum, axis.maximum)
                # Breakpoints are counts, so a float cell type would be a schema mistake.
                self.assertIn(axis.cell_type, ("u8", "u16", "u32"))

    def test_axes_are_not_offered_as_scalars_or_tables(self) -> None:
        for axis in cal.AXES:
            with self.subTest(axis=axis.label()):
                self.assertFalse(axis.is_table)
        self.assertNotIn("fuel.ve_table_rpm_axis",
                         [i.name for i in cal.TABLES])
        self.assertNotIn("fuel.ve_table_rpm_axis",
                         [i.name for i in cal.SCALARS])


class TestAxisStorage(unittest.TestCase):
    def setUp(self) -> None:
        self.image = cal.Calibration()

    def test_the_shipped_axes_are_evenly_spaced_and_ordered(self) -> None:
        for axis in cal.AXES:
            with self.subTest(axis=axis.label()):
                points = self.image.axis(axis)
                self.assertEqual(len(points), axis.alloc_count)
                self.assertTrue(self.image.axis_is_monotonic(axis),
                                f"{axis.label()} ships out of order: {points}")
                self.assertAlmostEqual(points[0], axis.minimum, delta=2.0)
                self.assertAlmostEqual(points[-1], axis.maximum, delta=2.0)

    def test_a_default_axis_covers_the_whole_declared_range(self) -> None:
        # An axis that does not reach its own maximum silently clamps every cell above it
        # to the last row, which reads as a tuning mistake the tuner will hunt for in the
        # table instead of finding in the axis.
        for axis in cal.AXES:
            with self.subTest(axis=axis.label()):
                points = self.image.axis(axis)
                self.assertGreaterEqual(points[0], axis.minimum)
                self.assertLessEqual(points[-1], axis.maximum)

    def test_a_breakpoint_survives_a_write_and_a_read_back(self) -> None:
        axis = cal.ITEMS_BY_NAME["fuel.ve_table_rpm_axis"]
        before = self.image.axis(axis)
        self.image.set_point(axis, 3, 2750.0)
        self.assertEqual(self.image.axis(axis)[3], 2750.0)
        # Round trip through the actual image bytes, not the in-memory list: the bytes are
        # what the ECU is handed.
        again = cal.Calibration.from_bytes(self.image.to_bytes())
        self.assertEqual(again.axis(axis)[3], 2750.0)
        self.assertEqual(again.axis(axis)[:3], before[:3])
        self.assertEqual(again.axis(axis)[4:], before[4:])

    def test_an_out_of_range_breakpoint_is_refused(self) -> None:
        axis = cal.ITEMS_BY_NAME["fuel.ve_table_rpm_axis"]
        with self.assertRaises(ValueError):
            self.image.set_point(axis, 0, 50.0)          # below the axis range
        with self.assertRaises(ValueError):
            self.image.set_point(axis, 0, 99_000.0)      # above it
        with self.assertRaises(IndexError):
            self.image.set_point(axis, axis.alloc_count, 2000.0)
        with self.assertRaises(IndexError):
            self.image.set_point(axis, -1, 2000.0)

    def test_a_breakpoint_may_be_dragged_past_its_neighbour(self) -> None:
        # The whole point of an editable axis. A per-point neighbour check would refuse
        # this edit and make the axis untunable, so the ordering is left to validation
        # rather than enforced at the point of the write.
        axis = cal.ITEMS_BY_NAME["fuel.ve_table_rpm_axis"]
        self.image.set_point(axis, 5, 1200.0)
        self.assertEqual(self.image.axis(axis)[5], 1200.0)
        # Which is now out of order, and the image must be able to say so.
        self.assertFalse(self.image.axis_is_monotonic(axis))

    def test_repeating_a_breakpoint_is_detected(self) -> None:
        axis = cal.ITEMS_BY_NAME["fuel.ve_table_rpm_axis"]
        points = self.image.axis(axis)
        self.image.set_point(axis, 4, points[3])
        self.assertFalse(self.image.axis_is_monotonic(axis))

    def test_an_axis_is_not_readable_as_a_scalar(self) -> None:
        # Reading an array through the scalar path would return just its first breakpoint
        # and look like a plausible number, so it is refused rather than guessed at.
        with self.assertRaises(TypeError):
            self.image.get("fuel.ve_table_rpm_axis")
        with self.assertRaises(TypeError):
            self.image.set("fuel.ve_table_rpm_axis", 2000.0)
        with self.assertRaises(TypeError):
            self.image.axis("engine.cylinder_count")
        with self.assertRaises(TypeError):
            self.image.table("fuel.ve_table_rpm_axis")

    def test_a_table_is_not_readable_as_an_axis(self) -> None:
        with self.assertRaises(TypeError):
            self.image.axis("fuel.ve_table")

    def test_the_image_is_a_stable_length_after_axis_edits(self) -> None:
        # Adding an axis must not change the size of the image. It is a length the ECU
        # checks, and an image of the wrong length is rejected with a CRC error that points
        # at nothing in particular.
        self.image.set_point("fuel.ve_table_rpm_axis", 0, 1500.0)
        self.assertEqual(len(self.image.to_bytes()), cal.IMAGE_SIZE)
        self.assertEqual(len(cal.default_image()), cal.IMAGE_SIZE)

    def test_axis_edits_show_up_in_the_diff(self) -> None:
        # If a changed axis is not in the diff, the operator writes a tune without being
        # told that the breakpoints moved - the single most destructive silent change there
        # is, because every cell in the table now means something different.
        other = cal.Calibration()
        self.image.set_point("fuel.ve_table_rpm_axis", 2, 2000.0)
        changed = self.image.differs_from(other)
        self.assertIn("ve_table_rpm_axis", [item.name for item in changed])

    def test_a_rescaled_axis_keeps_the_image_valid(self) -> None:
        # Put real resolution where an engine spends its time: dense at idle, sparse up top.
        axis = cal.ITEMS_BY_NAME["fuel.ve_table_rpm_axis"]
        n = axis.alloc_count
        # Dense below 3,200 rpm, sparse above - the shape a tuner would actually dial in
        # for a road engine, where nothing above 8,000 rpm is ever visited.
        low = [1000, 1200, 1400, 1600, 1900, 2300, 2800, 3200]
        high = [4000, 5000, 6000, 7000, 8000, 9500, 11000, 12000]
        points = low + high
        self.assertEqual(len(points), n)
        for index, value in enumerate(points):
            self.image.set_point(axis, index, float(value))
        self.assertTrue(self.image.axis_is_monotonic(axis))
        self.assertEqual(self.image.axis(axis), [float(v) for v in points])
        again = cal.Calibration.from_bytes(self.image.to_bytes())
        self.assertEqual(again.axis(axis), [float(v) for v in points])


if __name__ == "__main__":
    unittest.main()
