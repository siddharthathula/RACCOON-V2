"""End-to-end tests: the real tuner against the real ECU, over the real protocol.

These are the tests that would have caught everything the unit tests could not. Each defect
this project has found in the integration - the unlock that accepted any client, the bulk
read whose reply could not be framed, the live frame that transmitted zeros where it
declared sensor readings - was invisible to a unit test on either side and obvious here.

No mocks and no stubs. The far end is a compiled ``rc_sim`` running the firmware's own
control loop, flash layer, validator and link service, reached over TCP with the same
transport the bench uses. A Python reimplementation of the ECU would be a second ECU, free
to agree with whatever the client expects, and could not find a single one of those.
"""

from __future__ import annotations

import os
import sys
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
# Absolute, because pytest is routinely run from a different working directory than the one
# that makes a relative path resolve - and a test suite that only imports when launched from
# one particular shell is a suite people stop running.
sys.path.insert(0, os.path.join(_ROOT, "tuner"))

from raccoon import calibration as cal  # noqa: E402
from raccoon.bridge import SimulatorBridge  # noqa: E402
from raccoon.client import EcuClient, NotAuthenticated, ResultError  # noqa: E402
from raccoon.transport import TcpTransport  # noqa: E402

UNLOCK_CODE = "raccoon"
WRONG_CODE = "not-the-code"

# Long enough for the plant to crank and settle, short enough to keep the suite quick.
SETTLE_SECONDS = 5.0


def engine_running(client: EcuClient) -> bool:
    """Wait until the ECU reports itself armed, which is what the flash gate looks at.

    Polling for the state rather than sleeping a fixed time: the engine is a simulation, so
    how long it takes to start is a property of the machine the tests run on, and a fixed
    sleep is either flaky on a slow host or needlessly slow on a fast one.
    """
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        live = client.latest_live(0.4)
        if live is not None and live.armed:
            return True
    return False


class EcuTestCase(unittest.TestCase):
    """One simulator for the whole class: starting a 3-second crank per test is wasteful,
    and several of these tests need the engine in different states anyway."""

    bridge: SimulatorBridge
    transport: TcpTransport

    @classmethod
    def setUpClass(cls) -> None:
        cls.bridge = SimulatorBridge(log_path=os.path.join(_HERE, "simulator.log"))
        # Launched, not connected: the simulator serves one client at a time, and a handle
        # opened here and abandoned would still hold the ECU's only endpoint.
        cls.port = cls.bridge.launch()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.bridge.stop()

    def setUp(self) -> None:
        self.bridge.start_engine()
        # One client per test, closed at the end. The link serves a single endpoint, so a
        # client left open would be replaced by the next test's - which works, but only by
        # accident, and the first test to notice would be the one debugging it.
        self.client = self.connected()
        self.addCleanup(self.client.disconnect)
        self.assertTrue(engine_running(self.client), "the simulated engine never started")

    def connected(self) -> EcuClient:
        client = EcuClient(TcpTransport("127.0.0.1", self.port, timeout=2.0), timeout=2.0)
        client.connect()
        # Logging is per session, so every test asks for it. Relying on a previous test
        # having left it on is order-dependent in a way that only shows up when the suite is
        # run with a different ordering.
        client.set_logging(20)
        return client

    def unlock(self) -> EcuClient:
        """Unlock the test's own client and hand it back.

        Deliberately not "open a new client and unlock that". The link serves ONE endpoint,
        and a new connection replaces the previous one - which is what a single-endpoint USB
        or CAN link really does. So a test that opened a second client silently evicted the
        first, and then the first's reads failed with a message about the ECU closing a link
        it had never closed. One client per test, as the hardware has.
        """
        self.client.authenticate(UNLOCK_CODE)
        return self.client

    def connected_and_unlocked(self) -> EcuClient:
        return self.unlock()

    def connected_locked(self) -> EcuClient:
        return self.client


class TestConnect(EcuTestCase):
    def test_reports_an_identity_that_matches_this_build(self) -> None:
        client = self.connected_locked()
        self.assertEqual(client.info.image_size, cal.IMAGE_SIZE)
        self.assertEqual(client.info.item_count, len(cal.ITEMS))
        self.assertEqual(len(client.info.salt), 16)
        self.assertGreater(client.info.firmware_version, 0)

    def test_publishes_the_salt_so_no_guess_is_needed(self) -> None:
        client = self.connected_locked()
        # A tuner that had to guess a salt could not unlock on the first attempt, and the
        # chicken-and-egg would be invisible: it would look like a wrong code.
        self.assertTrue(client.salt)
        self.assertTrue(client.proof(UNLOCK_CODE))


class TestUnlock(EcuTestCase):
    def test_a_locked_ecu_refuses_a_write(self) -> None:
        client = self.connected_locked()
        with self.assertRaises(NotAuthenticated):
            client.write_item(cal.ITEMS_BY_NAME["engine.cylinder_count"], 3.0)

    def test_the_wrong_code_is_refused(self) -> None:
        client = self.connected_locked()
        with self.assertRaises(ResultError) as caught:
            client.authenticate(WRONG_CODE)
        self.assertFalse(client.authenticated)
        self.assertIn("locked", str(caught.exception).lower())

    def test_the_right_code_unlocks(self) -> None:
        client = self.connected_locked()
        client.authenticate(UNLOCK_CODE)
        self.assertTrue(client.authenticated)


class TestReadWrite(EcuTestCase):
    def test_reads_the_whole_image(self) -> None:
        client = self.connected_and_unlocked()
        self.addCleanup(client.disconnect)
        image = client.read_bulk()
        self.assertEqual(len(image.to_bytes()), cal.IMAGE_SIZE)
        self.assertEqual(
            image.to_bytes(), cal.default_image(),
            "a freshly started ECU should be running the shipped defaults",
        )

    def test_a_read_reports_progress_that_reaches_the_end(self) -> None:
        client = self.unlock()
        seen: list[tuple[int, int]] = []
        client.read_bulk(on_progress=lambda done, total: seen.append((done, total)))
        self.assertTrue(seen)
        self.assertEqual(seen[-1][0], seen[-1][1])
        self.assertEqual(seen[-1][1], cal.IMAGE_SIZE)

    def test_refuses_a_write_while_the_engine_is_running(self) -> None:
        client = self.unlock()
        # The engine is running: this is the single most important refusal in the project.
        with self.assertRaises(ResultError) as caught:
            client.stage(client.read_bulk().to_bytes())
        self.assertIn("running", str(caught.exception).lower())


class TestAxisOverTheWire(EcuTestCase):
    """An axis edited from the tuner, all the way into the ECU's working image.

    This is the path that has to work before a table editor is worth opening: the operator
    drags a breakpoint, and either the ECU holds the new value or the change is a lie drawn
    on the screen while the ECU keeps the old axis.
    """

    AXIS = "fuel.ve_table_rpm_axis"

    def test_a_shipped_axis_reads_back_from_the_ecu(self) -> None:
        client = self.unlock()
        image = client.read_bulk()
        self.assertTrue(image.axis_is_monotonic(self.AXIS))
        expected = cal.ITEMS_BY_NAME[self.AXIS]
        self.assertEqual(len(image.axis(self.AXIS)), expected.alloc_count)
        # And point by point, over the link, not just out of the bulk image.
        for index in range(expected.alloc_count):
            self.assertEqual(
                client.read_item(expected.id, index), image.axis(self.AXIS)[index],
                f"breakpoint {index} disagrees between the bulk read and a single read",
            )

    def test_moving_a_breakpoint_reaches_the_ecu_and_survives_a_read_back(self) -> None:
        client = self.unlock()
        image = client.read_bulk()
        before = image.axis(self.AXIS)
        self.bridge.stop_engine()
        try:
            # Put real resolution where the engine lives: dense low, sparse high.
            points = [1000.0, 1200.0, 1400.0, 1600.0, 1900.0, 2300.0, 2800.0, 3200.0,
                      4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9500.0, 11000.0, 12000.0]
            self.assertEqual(len(points), len(before))
            for index, value in enumerate(points):
                client.write_axis_point(cal.ITEMS_BY_NAME[self.AXIS], index, value)
            again = client.read_bulk()
            self.assertEqual(again.axis(self.AXIS), points)
            self.assertTrue(again.axis_is_monotonic(self.AXIS))
        finally:
            self.bridge.start_engine()

    def test_a_breakpoint_outside_the_axis_is_refused(self) -> None:
        client = self.unlock()
        self.bridge.stop_engine()
        try:
            axis = cal.ITEMS_BY_NAME[self.AXIS]
            with self.assertRaises(ResultError):
                client.write_axis_point(axis, 0, 50.0)
            with self.assertRaises(ResultError):
                client.write_axis_point(axis, 0, 99_000.0)
        finally:
            self.bridge.start_engine()

    def test_a_breakpoint_past_the_end_of_the_axis_is_refused(self) -> None:
        client = self.unlock()
        self.bridge.stop_engine()
        try:
            axis = cal.ITEMS_BY_NAME[self.AXIS]
            with self.assertRaises(ResultError):
                client.write_axis_point(axis, axis.alloc_count, 2000.0)
        finally:
            self.bridge.start_engine()

    def test_an_axis_edited_into_disorder_is_refused_before_it_can_be_written(self) -> None:
        # The per-point write does NOT check its neighbours, on purpose: dragging a
        # breakpoint past the one next to it is the edit a tuner makes most often. The ECU
        # has to catch the result at validation, or the table below it is unsearchable.
        client = self.unlock()
        self.bridge.stop_engine()
        try:
            axis = cal.ITEMS_BY_NAME[self.AXIS]
            with self.assertRaises(ResultError) as caught:
                client.write_axis_point(axis, 5, 1200.0)  # below breakpoint 4
            # The refusal has to SAY WHY, and it has to say the right why. Every breakpoint
            # here is inside its declared range, so "the value is outside the range that
            # item allows" would be a confident lie that sends the operator looking for a
            # range limit that does not exist.
            message = str(caught.exception).lower()
            self.assertIn("increase", message,
                          f"the refusal does not explain the ordering: {message}")
            self.assertNotIn("outside the range", message)
            # The working image is untouched: a refused write must not leave a partial edit.
            self.assertTrue(client.read_bulk().axis_is_monotonic(self.AXIS))
        finally:
            self.bridge.start_engine()

    def test_a_scalar_cannot_be_written_with_an_axis_index(self) -> None:
        # Target 2 means "this is a breakpoint". Applying it to a scalar must be refused
        # rather than quietly writing the scalar.
        client = self.unlock()
        self.bridge.stop_engine()
        try:
            scalar = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
            before = client.read_bulk().get(scalar)
            with self.assertRaises(ResultError):
                client.write_axis_point(scalar, 0, 1500.0)
            self.assertEqual(client.read_bulk().get(scalar), before)
        finally:
            self.bridge.start_engine()

    def test_an_axis_is_not_read_through_a_table_cell_index(self) -> None:
        client = self.unlock()
        axis = cal.ITEMS_BY_NAME[self.AXIS]
        # Index 0 is valid for an axis and also valid for a 16-cell table, so the reply has
        # to come from the axis on both sides - which it does, because the ECU dispatches on
        # the item id rather than on the index.
        self.assertEqual(client.read_item(axis.id, 0), 1000.0)

    def test_every_table_ships_with_both_of_its_axes_over_the_link(self) -> None:
        client = self.unlock()
        image = client.read_bulk()
        for table in cal.TABLES:
            with self.subTest(table=table.label()):
                self.assertIsNotNone(table.row_axis_id, "no row axis")
                self.assertIsNotNone(table.col_axis_id, "no column axis")
                row = image.row_axis_of(table)
                col = image.col_axis_of(table)
                self.assertEqual(len(row), table.alloc_rows)
                self.assertEqual(len(col), table.alloc_cols)
                self.assertTrue(image.axis_is_monotonic(table.row_axis_id))
                self.assertTrue(image.axis_is_monotonic(table.col_axis_id))


class TestFlashWorkflow(EcuTestCase):
    """Flashing changes the ECU's stored calibration.

    It does not, in the simulator, change the tune the plant is actually running: the harness
    holds its own engine geometry and the link service holds a separate working image. That
    split is deliberate - it is what lets a flash workflow be tested on an engine that is
    running, and then again with the engine stopped, without the two interfering. It also
    means these tests do not need to restore anything afterwards.
    """

    def stop_engine(self) -> None:
        """Stop the plant and wait for the ECU to agree.

        Polling the ECU's own report rather than trusting the control port's "stopped" is
        deliberate: the flash gate reads the ECU's state, so a rig that says it stopped while
        the ECU still believes the engine is running produces a confusing refusal rather
        than a clear one.
        """
        self.assertEqual(self.bridge.stop_engine(), "stopped")
        deadline = time.monotonic() + 10.0
        last = None
        quiet = 0
        while time.monotonic() < deadline:
            last = self.client.latest_live(0.4)
            if last is None:
                # The link went away. That is a different failure from "the engine is still
                # running", and conflating them sends the reader to the ECU instead of to
                # the connection.
                quiet += 1
                if quiet > 5:
                    # Distinguish "no live data" from "the link is gone": the first means
                    # logging is off, the second means the ECU or the rig has died, and they
                    # need completely different investigations.
                    try:
                        self.client._request(0x01)   # PING
                        state = "the link answers, but no live data is arriving"
                    except Exception as exc:            # noqa: BLE001 - the point is the message
                        state = f"the link itself is failing: {exc}"
                    self.fail(f"the link went quiet while waiting for the engine to stop: {state}")
                continue
            quiet = 0
            if not last.armed:
                return
        self.fail(
            f"the ECU still reports the engine as running after it was stopped: "
            f"armed={last.armed} rpm={last.rpm:.0f} sync={last.sync_state}"
        )

    def test_a_stopped_ecu_accepts_a_tune_and_serves_it_afterwards(self) -> None:
        client = self.unlock()
        self.stop_engine()

        edited = client.read_bulk()
        edited.set("engine.cylinder_count", 3)
        edited.set_cell("fuel.ve_table", 0, 74.0)
        client.flash_calibration(edited, UNLOCK_CODE)

        after = client.read_bulk()
        self.assertEqual(after.get("engine.cylinder_count"), 3.0)
        self.assertEqual(after.table("fuel.ve_table")[0], 74.0)

    def test_a_rollback_returns_to_the_previous_tune(self) -> None:
        client = self.unlock()
        self.stop_engine()

        first = client.read_bulk()
        first.set("engine.cylinder_count", 3)
        client.flash_calibration(first, UNLOCK_CODE)
        self.assertEqual(client.read_bulk().get("engine.cylinder_count"), 3.0)

        second = client.read_bulk()
        second.set("engine.cylinder_count", 4)
        client.flash_calibration(second, UNLOCK_CODE)
        self.assertEqual(client.read_bulk().get("engine.cylinder_count"), 4.0)

        client.rollback()
        # The ECU now SERVES the older tune, not just reports having switched to it. A
        # service that kept its own working copy would report success and then hand back the
        # tune the operator just rolled away from - and their next move would be to try the
        # rollback again.
        self.assertEqual(client.read_bulk().get("engine.cylinder_count"), 3.0)

class TestLiveData(EcuTestCase):
    def test_live_frames_describe_a_running_engine(self) -> None:
        client = self.unlock()
        client.set_logging(20)
        live = client.latest_live(2.0)
        self.assertIsNotNone(live)
        self.assertGreater(live.rpm, 300.0)
        self.assertEqual(live.sync_state, 4, "expected the crank trigger to be synced")
        self.assertEqual(live.armed, 1)

    def test_declared_sensor_fields_carry_readings(self) -> None:
        client = self.unlock()
        client.set_logging(20)
        live = client.latest_live(2.0)
        # These four were declared in the frame and hard-coded to zero. A field that
        # promises a reading and transmits a constant looks exactly like a disconnected
        # sensor, so a diagnostic built on it reaches the wrong conclusion confidently.
        self.assertGreater(live.battery_v, 10.0, "battery voltage reads zero")
        self.assertGreater(live.fuel_pressure_kpa, 100.0, "fuel pressure reads zero")
        self.assertGreater(live.ect_c, 10.0, "coolant temperature reads zero")
        self.assertGreater(live.iat_c, -20.0, "intake temperature reads an implausible value")


if __name__ == "__main__":
    unittest.main(verbosity=2)
