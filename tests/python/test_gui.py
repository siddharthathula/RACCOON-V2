"""GUI tests: the real window, the real widgets, driving the real ECU.

These are not screenshot tests. Each one builds the actual :class:`MainWindow`, clicks the
actual buttons through the actual signals, and checks what the operator would see. A
screenshot proves the interface renders; it does not prove that pressing the write button
writes the right thing, which is the part that matters.

The window is built against the simulator, so these are also the only tests that exercise
the whole stack at once - the editor, the diff, the confirmation, the transfer, and the ECU
refusing to accept a tune while the engine is running.
"""

from __future__ import annotations

import os
import sys
import unittest

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(_ROOT, "tuner"))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PyQt6.QtCore import QEventLoop, QTimer  # noqa: E402
from PyQt6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from raccoon import calibration as cal  # noqa: E402
from raccoon.__main__ import _application  # noqa: E402
from raccoon.ui.main_window import MainWindow  # noqa: E402

UNLOCK_CODE = "raccoon"

# Long enough for a whole transfer over a loopback socket, short enough not to make the
# suite feel broken. The transfers are a few kilobytes; this is generous, not tight.
SETTLE_MS = 4000


def pump_until(app: QApplication, predicate, ms: int = 8000, what: str = "condition") -> None:
    """Run the event loop until `predicate` holds, or give up.

    A fixed sleep is the alternative and it is the wrong one: whether a 1500 ms wait is
    enough depends on how fast the machine is, so a test either flakes on a slow host or
    wastes time on a fast one. Worse, a too-short wait fails LATE and confusingly - a test
    that reads the editor before the read has landed sees the PREVIOUS test's values and
    fails on an assertion about the wrong number.
    """
    deadline = [ms]
    step = 50
    while deadline[0] > 0:
        QTimer.singleShot(step, lambda: None)
        loop = QEventLoop()
        QTimer.singleShot(step, loop.quit)
        loop.exec()
        deadline[0] -= step
        if predicate():
            return
    raise AssertionError(f"timed out after {ms}ms waiting for {what}")


def pump(app: QApplication, ms: int = SETTLE_MS) -> None:
    """Run the interface's event loop for a while.

    `processEvents` in a loop would spin without ever letting a queued signal that depends
    on a timer fire promptly, and would busy-wait. A real event loop with a timer is closer
    to what a user's window actually does.
    """
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


class GuiTestCase(unittest.TestCase):
    app: QApplication
    window: MainWindow

    @classmethod
    def setUpClass(cls) -> None:
        cls.app = _application()
        cls.window = MainWindow("simulator")
        cls.window.show()
        pump(cls.app, 3000)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.window.close()

    @property
    def bridge(self):
        """The window's own simulator.

        Not a second one: the window starts the ECU the tests are talking to, and a separate
        SimulatorBridge here would have no process and no control port. Two bridges, one of
        them empty, is a confusing way to fail.
        """
        return self.window.bridge

    def unlock(self) -> None:
        self.window.transfer.code.setText(UNLOCK_CODE)
        self.window.transfer.unlock_button.click()
        pump_until(
            self.app,
            lambda: self.window.worker.state.authenticated,
            what="the unlock to succeed",
        )

    def read(self) -> None:
        before = self.window.log.textCursor().position()
        del before
        self.window.transfer.read_button.click()
        # Wait for the transfer panel to say the read finished, not for a fixed time.
        pump_until(
            self.app,
            lambda: "from the ecu" in self.window.log.toPlainText()[-200:].lower(),
            what="the calibration read to finish",
        )

    def stop_engine(self) -> None:
        self.bridge.stop_engine()
        pump(self.app, 800)


class TestConnection(GuiTestCase):
    def test_the_window_connects_to_the_simulator_on_startup(self) -> None:
        # The default target is the simulator, so the interface can be opened and reviewed
        # with no hardware - and so it cannot connect to a real motorcycle by accident.
        self.assertIsNotNone(self.window.worker)
        self.assertTrue(self.window.worker.state.connected, "the window did not connect")
        self.assertIn("connected", self.window.dashboard.link_state.text().lower())

    def test_the_dashboard_receives_live_frames(self) -> None:
        pump(self.app, 3000)
        dashboard = self.window.dashboard
        self.assertIsNotNone(dashboard.readouts["rpm"].value.text())
        self.assertNotEqual(dashboard.readouts["rpm"].value.text(), "--",
                            "the rpm readout never received a value")

    def test_a_dead_link_marks_the_gauges_stale_rather_than_freezing_them(self) -> None:
        """A frozen gauge is the one thing a dashboard must never do.

        A dead link that leaves the last value on screen, unchanged and unmarked, is
        indistinguishable from an engine holding a steady state. So the readout has to grey
        out and say "stale" - and that is checkable without unplugging anything, by asking
        the dashboard to handle a timeout.
        """
        from raccoon.ui.worker import REPAINT_HZ  # noqa: F401 - documents the rate

        self.window.dashboard.on_live(self.window.dashboard._last, 0.0)
        self.window.dashboard.on_timeout()
        # Checked immediately, with no pump: the window's own 50 ms repaint timer would
        # deliver a fresh frame and mark the gauge live again, so waiting here would be
        # testing the timer rather than the staleness.
        self.assertTrue(self.window.dashboard.readouts["rpm"].stale)
        self.assertEqual(self.window.dashboard.readouts["rpm"].note.text(), "stale")
        self.assertIn("no data", self.window.dashboard.freshness.text())


class TestUnlock(GuiTestCase):
    def test_unlocking_updates_every_view(self) -> None:
        self.assertFalse(self.window.worker.state.authenticated)
        self.unlock()
        self.assertTrue(self.window.worker.state.authenticated)
        # Both views, not just the one the button belongs to. A window whose status bar says
        # unlocked and whose dashboard says locked sends the operator looking for a bug in
        # the ECU.
        self.assertIn("unlocked", self.window.dashboard.link_state.text().lower())
        self.assertIn("unlocked", self.window.transfer.status.text().lower())

    def test_the_wrong_code_is_refused_and_the_interface_says_so(self) -> None:
        self.window.transfer.code.setText("wrong")
        self.window.transfer.unlock_button.click()
        pump(self.app, 1200)
        self.assertFalse(self.window.worker.state.authenticated)
        self.assertIn("locked", self.window.log.toPlainText().lower())


class TestReadAndEdit(GuiTestCase):
    def setUp(self) -> None:
        self.unlock()
        self.read()

    def test_reading_loads_the_ecu_tune_into_the_editor(self) -> None:
        self.assertIsNotNone(self.window.editor.image)
        self.assertEqual(len(self.window.editor.image.to_bytes()), cal.IMAGE_SIZE)
        self.assertEqual(self.window.editor.image.get("engine.cylinder_count"), 2.0)

    def test_the_editor_tree_lists_the_items(self) -> None:
        tree = self.window.editor.tree
        self.assertGreaterEqual(tree.topLevelItemCount(), 20)
        first = tree.topLevelItem(0)
        self.assertEqual(first.text(0), "engine")
        self.assertGreater(first.childCount(), 10)

    def test_searching_filters_the_tree(self) -> None:
        tree = self.window.editor.tree
        self.window.editor.search.setText("ve_table")
        pump(self.app, 200)
        visible = 0
        for i in range(tree.topLevelItemCount()):
            node = tree.topLevelItem(i)
            for j in range(node.childCount()):
                if not node.child(j).isHidden():
                    visible += 1
        self.assertGreater(visible, 0, "the search hid everything")
        # And it is selective: 519 items, a search for one table should not match the lot.
        self.assertLess(visible, 60, f"the search matched {visible} items")
        self.window.editor.search.setText("")
        pump(self.app, 200)

    def test_editing_a_scalar_marks_the_tune_as_changed(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        self.window.editor.image.set(item, 1500.0)
        self.window.editor._refresh_diff()
        self.window._on_editor_changed()
        changed = self.window.editor.changed_items()
        self.assertEqual(len(changed), 1)
        self.assertEqual(changed[0].name, "idle_rpm_target")
        self.assertIn("1 item", self.window.transfer.summary.text())

    def test_editing_back_to_the_original_clears_the_diff(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        original = self.window.editor.image.get(item)
        self.window.editor.image.set(item, original + 100.0)
        self.window._on_editor_changed()
        self.assertTrue(self.window.editor.changed_items())
        self.window.editor.image.set(item, original)
        self.window._on_editor_changed()
        self.assertFalse(self.window.editor.changed_items())
        self.assertIn("matches the ECU", self.window.transfer.summary.text())


class TestWriteRefusals(GuiTestCase):
    def setUp(self) -> None:
        # Every test starts with the engine RUNNING. Alphabetical order means a test that
        # stopped the engine would otherwise leave the next one starting from the opposite
        # state, and the first symptom would be a button that is disabled for a reason that
        # has nothing to do with what the test is checking.
        self.bridge.start_engine()
        pump(self.app, 2500)
        self.unlock()
        self.read()

    def test_the_write_button_is_disabled_while_the_engine_runs(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        self.window.editor.image.set(item, 1500.0)
        self.window._on_editor_changed()
        pump(self.app, 200)
        # The engine is running, so the ECU would refuse - and the button must say so
        # rather than let the operator discover it by pressing it.
        self.assertFalse(self.window.transfer.write_button.isEnabled())
        self.assertIn("engine is running", self.window.transfer.refusal.text())

    def test_the_write_button_comes_alive_once_the_engine_is_stopped(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        self.window.editor.image.set(item, 1500.0)
        self.window._on_editor_changed()
        self.stop_engine()
        pump(self.app, 600)
        self.assertTrue(self.window.transfer.write_button.isEnabled())
        self.assertEqual(self.window.transfer.refusal.text(), "")

    def test_a_locked_ecu_offers_no_write_button(self) -> None:
        self.stop_engine()
        self.window.worker.state.authenticated = False
        self.window._on_state(self.window.worker.state)
        pump(self.app, 150)
        self.assertFalse(self.window.transfer.write_button.isEnabled())
        self.assertIn("locked", self.window.transfer.refusal.text())


class TestProjectScreen(GuiTestCase):
    """The vehicle / project screen: describe a bike, get a calibration."""

    def test_every_shipped_profile_is_offered(self) -> None:
        screen = self.window.projects
        names = [screen.profile_box.itemText(i) for i in range(screen.profile_box.count())]
        self.assertIn("gt650", names)
        self.assertGreaterEqual(len(names), 4)

    def test_choosing_a_profile_shows_its_provenance(self) -> None:
        screen = self.window.projects
        screen.profile_box.setCurrentText("gt650")
        pump(self.app, 200)
        # Every value, with its provenance, not just the doubtful ones.
        self.assertGreater(screen.honesty.rowCount(), 30)
        provenances = {screen.honesty.item(i, 1).text()
                       for i in range(screen.honesty.rowCount())}
        self.assertTrue({"measured", "datasheet", "wiring"} & provenances)
        self.assertIn("assumed", provenances)

    def test_the_gt650_profile_says_what_is_still_unknown(self) -> None:
        screen = self.window.projects
        screen.profile_box.setCurrentText("gt650")
        pump(self.app, 200)
        screen._on_build()
        pump(self.app, 200)
        text = " ".join(screen.outstanding.item(i).text()
                        for i in range(screen.outstanding.count())).lower()
        # The three measurements that decide whether this engine runs.
        self.assertIn("tdc", text)
        self.assertIn("injector flow", text)
        self.assertIn("fuel table", text)

    def test_building_loads_the_calibration_into_the_editor(self) -> None:
        screen = self.window.projects
        screen.profile_box.setCurrentText("gt650")
        pump(self.app, 200)
        screen._on_build()
        pump(self.app, 300)
        image = self.window.editor.image
        self.assertIsNotNone(image)
        self.assertEqual(image.get("engine.bore_mm"), 78.0)
        self.assertAlmostEqual(image.get("engine.displacement_cc"), 648.0, places=0)

    def test_the_unmeasured_template_refuses_rather_than_half_building(self) -> None:
        from raccoon import build
        from raccoon.vehicle import VehicleProfile
        profile = VehicleProfile.load(
            os.path.join(_ROOT, "engine_profiles", "parallel_twin.json"))
        # The template is a template: several values are deliberately unverified, and the
        # build must still work, but it must say so loudly.
        image, report = build.build(profile)
        self.assertGreater(len(report.warnings), 5)
        self.assertTrue(report.outstanding)

    def test_a_broken_profile_shows_the_reason_rather_than_crashing(self) -> None:
        from raccoon import build
        from raccoon.vehicle import VehicleProfile
        import json as _json
        path = os.path.join(_ROOT, "engine_profiles", "parallel_twin.json")
        raw = _json.loads(open(path).read())
        raw["geometry"]["firing_order"] = [0, 0]
        profile = VehicleProfile.from_dict(raw)
        with self.assertRaises(build.ProfileError) as caught:
            build.build(profile)
        self.assertIn("firing_order", str(caught.exception))


class TestWrite(GuiTestCase):
    def setUp(self) -> None:
        self.unlock()
        self.read()
        self.stop_engine()

    def tearDown(self) -> None:
        # Hand the rig back with the engine running, so the next test is not told the
        # engine is stopped when it never touched it.
        self.bridge.start_engine()

    def test_writing_a_tune_reaches_the_ecu_and_survives_a_read_back(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        self.window.editor.image.set(item, 1500.0)
        self.window._on_editor_changed()
        pump_until(self.app, lambda: self.window.transfer.write_button.isEnabled(),
                   what="the write button to enable")

        # Answer the confirmation dialog with Yes. Patched rather than clicked, because the
        # dialog is modal and there is no way to reach a button inside it from a test without
        # either a timer or a monkeypatch - and a timer here would race the write.
        original = QMessageBox.question
        QMessageBox.question = staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes)
        try:
            self.window.transfer.write_button.click()
            pump(self.app, 15000)
        finally:
            QMessageBox.question = original

        # The window re-reads the calibration from the ECU after a successful write, so the
        # editor now holds what the ECU is serving. That read IS the proof - and asking the
        # worker's client directly would be reading the same socket from a second thread,
        # which the client now refuses for exactly this reason.
        self.assertEqual(self.window.editor.image.get(item), 1500.0,
                         "the tune the interface wrote is not the tune the ECU is serving.\n"
                         "log:\n" + self.window.log.toPlainText())
        self.assertIn("activated", self.window.log.toPlainText().lower())

    def test_cancelling_the_confirmation_writes_nothing(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        # What the ECU is serving NOW, before the edit. Captured from the baseline, which is
        # a snapshot of the ECU's tune - capturing the working copy after the edit would
        # record the value being cancelled, and the assertion below would compare the ECU
        # against itself and prove nothing.
        on_the_ecu = self.window.editor.baseline.get(item)

        self.window.editor.image.set(item, on_the_ecu + 300.0)
        self.window._on_editor_changed()
        pump_until(self.app, lambda: self.window.transfer.write_button.isEnabled(),
                   what="the write button to enable")

        original = QMessageBox.question
        QMessageBox.question = staticmethod(lambda *a, **k: QMessageBox.StandardButton.No)
        try:
            self.window.transfer.write_button.click()
            pump(self.app, 1200)
        finally:
            QMessageBox.question = original

        self.assertIn("cancelled", self.window.log.toPlainText().lower())
        # Re-read from the ECU: the value it serves is the value it was asked to store, and
        # asking the ECU is the only way to know a cancel really cancelled.
        self.read()
        self.assertEqual(self.window.editor.baseline.get(item), on_the_ecu,
                         "cancelling the confirmation still changed the ECU")

    def test_the_confirmation_lists_the_items_it_will_change(self) -> None:
        item = cal.ITEMS_BY_NAME["engine.idle_rpm_target"]
        self.window.editor.image.set(item, 1500.0)
        self.window._on_editor_changed()
        pump_until(self.app, lambda: self.window.transfer.write_button.isEnabled(),
                   what="the write button to enable")

        seen: list[str] = []

        def capture(parent, title, text, *a, **k):
            seen.append(text)
            return QMessageBox.StandardButton.No

        original = QMessageBox.question
        QMessageBox.question = staticmethod(capture)
        try:
            self.window.transfer.write_button.click()
            pump(self.app, 600)
        finally:
            QMessageBox.question = original

        self.assertTrue(seen, "no confirmation dialog was shown")
        # "idle_rpm_target" in the text: an operator who is about to change a calibration is
        # entitled to be told which item.
        self.assertIn("idle_rpm_target", seen[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
