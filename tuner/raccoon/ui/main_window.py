"""The main window: tabs, a log, and the wiring between them.

The window owns no protocol logic. Everything slow or fallible happens on the link worker's
thread, and this file's whole job is to route signals to slots and keep the three views
agreeing with each other.

A log panel sits at the bottom because a tuner that cannot tell you what it did is a tuner
you cannot use to work out why it did the wrong thing. Every connection, refusal, transfer
and error lands there with a timestamp, and the log is what a fault report is built from.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

from PyQt6.QtCore import QMetaObject, Qt, QThread, QTimer, pyqtSignal, pyqtSlot
from PyQt6.QtGui import QFont, QTextCursor
from PyQt6.QtWidgets import (
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from raccoon import calibration as cal
from raccoon.bridge import BridgeError, SimulatorBridge
from raccoon.client import EcuClient
from raccoon.transport import SerialTransport, TcpTransport, TransportError
from raccoon.ui.dashboard import Dashboard
from raccoon.ui.editor import CalibrationEditor
from raccoon.ui.project import ProjectScreen
from raccoon.ui.transfer import TransferPanel
from raccoon.ui.worker import LinkWorker

# A frame older than this is shown as stale. Three intervals is generous: a link that has
# missed three of its own frames is not "a moment behind", it is gone, and showing it as
# merely delayed is how an operator ends up trusting a frozen gauge.
STALE_AFTER_S = 0.5

STYLE = """
QWidget { background: #0b1120; color: #e2e8f0; }
QGroupBox {
    border: 1px solid #1e293b; border-radius: 4px; margin-top: 10px;
    font-weight: bold; padding-top: 6px;
}
QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #94a3b8; }
QPushButton {
    background: #1e293b; border: 1px solid #334155; border-radius: 3px;
    padding: 6px 12px;
}
QPushButton:hover:!disabled { background: #334155; }
QPushButton:disabled { color: #475569; border-color: #1e293b; }
QPushButton[accent="true"] { background: #0369a1; border-color: #0ea5e9; }
QLineEdit, QDoubleSpinBox, QTableWidget, QTreeWidget, QSpinBox {
    background: #0f172a; border: 1px solid #1e293b; border-radius: 3px;
    selection-background-color: #0369a1;
}
QHeaderView::section { background: #0f172a; color: #94a3b8; border: 0; padding: 4px; }
QProgressBar {
    border: 1px solid #1e293b; border-radius: 3px; text-align: center;
    background: #0f172a; color: #e2e8f0;
}
QProgressBar::chunk { background: #0ea5e9; }
QPlainTextEdit { background: #0f172a; border: 1px solid #1e293b; font-family: monospace; }
QTabWidget::pane { border: 1px solid #1e293b; }
QTabBar::tab {
    background: #0f172a; padding: 8px 16px; border: 1px solid #1e293b;
    border-bottom: 0;
}
QTabBar::tab:selected { background: #1e293b; color: #f8fafc; }
QSplitter::handle { background: #1e293b; }
"""


class MainWindow(QMainWindow):
    """The application window.

    **Every request to the link goes through a signal**, and every one of them is connected
    to the worker's slot with a queued connection. Calling a worker's method directly from
    here would run it on the interface thread - the worker's thread affinity only affects
    slots invoked through the event loop, not ordinary Python calls - so the blocking read
    inside ``connect`` would freeze the window at startup, and the failure would look like a
    hang rather than a threading mistake.

    Routing everything through signals makes the ownership rule mechanical: the interface
    thread only ever *asks*, and only ever receives results.
    """

    request_connect = pyqtSignal()
    request_disconnect = pyqtSignal()
    request_unlock = pyqtSignal(str)
    request_read = pyqtSignal()
    request_write = pyqtSignal()
    request_rollback = pyqtSignal()
    request_set_calibration = pyqtSignal(object)

    def __init__(self, target: str = "simulator") -> None:
        super().__init__()
        self.setWindowTitle("RACCOON V2 tuner")
        self.resize(1280, 860)
        self.setStyleSheet(STYLE)

        self.bridge: SimulatorBridge | None = None
        self.thread: QThread | None = None
        self.worker: LinkWorker | None = None
        self._last_frame_at = 0.0
        self._image: cal.Calibration | None = None

        self._build(target)
        self._start(target)

    # ------------------------------------------------------------------- build

    def _build(self, target: str) -> None:
        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(8, 8, 8, 8)

        header = QLabel(target)
        header.setStyleSheet("color:#64748b; font-size:11px;")
        layout.addWidget(header)

        split = QSplitter(Qt.Orientation.Vertical)

        self.tabs = QTabWidget()
        self.dashboard = Dashboard()
        self.editor = CalibrationEditor()
        self.transfer = TransferPanel()
        self.projects = ProjectScreen()
        self.tabs.addTab(self.dashboard, "dashboard")
        self.tabs.addTab(self.projects, "vehicle / project")
        self.tabs.addTab(self.editor, "calibration")
        self.tabs.addTab(self.transfer, "connect / write")
        split.addWidget(self.tabs)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(150)
        self.log.setFont(QFont("DejaVu Sans Mono", 9))
        split.addWidget(self.log)

        split.setStretchFactor(0, 4)
        split.setStretchFactor(1, 1)
        layout.addWidget(split)
        self.setCentralWidget(central)

        self.statusBar().showMessage("starting")

        # The repaint timer. It does not read the link - the worker does that - it only
        # decides whether the last frame is still recent enough to be worth showing, so a
        # dead link degrades to visibly stale gauges rather than a frozen confident one.
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._on_tick)
        self.timer.start(50)

    # -------------------------------------------------------------------- link

    def _make_transport(self, target: str):
        if target.startswith("simulator"):
            self.bridge = SimulatorBridge()
            return self.bridge.start()
        if target.startswith("tcp:"):
            host, _, port = target[4:].partition(":")
            return TcpTransport(host, int(port or 5000), timeout=2.0)
        if target.startswith("serial:"):
            return SerialTransport(target[7:], timeout=2.0)
        if target.startswith("com"):
            return SerialTransport(target, timeout=2.0)
        raise ValueError(
            f"unknown target {target!r}. Use 'simulator', 'tcp:host:port', "
            f"'serial:/dev/ttyUSB0' or a port name."
        )

    def _start(self, target: str) -> None:
        try:
            transport = self._make_transport(target)
        except (BridgeError, TransportError, ValueError) as exc:
            self._log(f"could not open {target}: {exc}", error=True)
            # A window that cannot start is still useful for reading the log, and closing it
            # immediately takes the explanation with it.
            self._log("no link is open. The dashboard is idle; nothing is being recorded.")
            self.statusBar().showMessage("no link")
            return

        self._shut_down = False
        self.thread = QThread(self)
        self.worker = LinkWorker(transport)
        self.worker.moveToThread(self.thread)
        self.thread.started.connect(self.worker.run)
        self.thread.start()

        # Queued, explicitly. A worker living on another thread makes this automatic, but
        # stating it means the connection cannot silently become direct if the worker is
        # ever moved back to this thread - which would freeze the window and would be very
        # hard to trace.
        self.request_connect.connect(self.worker.do_connect, Qt.ConnectionType.QueuedConnection)
        self.request_disconnect.connect(self.worker.do_disconnect, Qt.ConnectionType.QueuedConnection)
        self.request_unlock.connect(self.worker.do_unlock, Qt.ConnectionType.QueuedConnection)
        self.request_read.connect(self.worker.do_read_calibration, Qt.ConnectionType.QueuedConnection)
        self.request_write.connect(self.worker.do_flash, Qt.ConnectionType.QueuedConnection)
        self.request_rollback.connect(self.worker.do_rollback, Qt.ConnectionType.QueuedConnection)
        self.request_set_calibration.connect(
            self.worker.set_calibration, Qt.ConnectionType.QueuedConnection
        )

        self.worker.connected.connect(self._on_connected)
        self.worker.disconnected.connect(self._on_disconnected)
        # The same state goes to both the transfer panel and the dashboard. They are two
        # views of one thing, and updating only one of them produces a window whose status
        # bar says "connected" while its dashboard says "not connected" - which reads as a
        # bug in the ECU rather than in the wiring.
        self.worker.state_changed.connect(self._on_state)
        self.worker.live.connect(self._on_live)
        self.worker.calibration_loaded.connect(self._on_calibration)
        self.worker.calibration_reading.connect(self._on_reading)
        self.worker.flashing.connect(self.transfer.on_progress)
        self.worker.flashed.connect(self._on_flashed)
        self.worker.message.connect(self._on_message)
        self.worker.failed.connect(self._on_failed)
        self.worker.unlock_prompt.connect(self._on_unlock_prompt)

        self.transfer.connect_requested.connect(self._on_connect_clicked)
        self.transfer.disconnect_requested.connect(self.request_disconnect.emit)
        self.transfer.unlock_requested.connect(self._on_unlock_clicked)
        self.transfer.read_requested.connect(self._on_read_clicked)
        self.transfer.write_requested.connect(self._on_write_clicked)
        self.transfer.rollback_requested.connect(self._on_rollback_clicked)
        self.editor.changed.connect(self._on_editor_changed)
        # A profile-built calibration is loaded into the editor and becomes the thing the
        # Write button will send. That is the whole path: describe the bike, build, edit, write.
        self.projects.calibration_built.connect(self._on_profile_built)
        self.projects.status.connect(self._log)

        # Deferred by one event-loop turn, so the window is fully constructed before
        # anything tries to talk to the link. Connecting from inside the constructor means
        # a signal can arrive before every widget exists.
        # aboutToQuit, so a signal-driven exit tears the thread down exactly as the close
        # button does. Registered after the worker exists, so the slot has something to act
        # on when it fires.
        from PyQt6.QtWidgets import QApplication
        QApplication.instance().aboutToQuit.connect(self.shutdown)

        QTimer.singleShot(0, self._on_connect_clicked)

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt's name
        self.shutdown()
        super().closeEvent(event)

    def shutdown(self) -> None:
        """Shut the link thread down properly. Idempotent.

        A QThread destroyed while running takes the process with it, and the window closing
        on its own is not a reason to crash on the way out.

        Reachable from two directions, and both are needed: the close button, and
        ``aboutToQuit``. Without the second, a SIGTERM or Ctrl-C ends the Qt event loop
        while the worker is still polling a socket, and the process hangs on the way out
        with the child simulator still running - a tuner that cannot be killed cleanly and
        leaves an engine turning.
        """
        if getattr(self, "_shut_down", False):
            return
        self._shut_down = True
        if self.worker is not None:
            # Invoked ON the worker's thread, and waited for. A QTimer belongs to the thread
            # that created it: stopping it from here produces "Timers cannot be stopped from
            # another thread" and leaves it armed, so the worker keeps reading the link after
            # the window is gone. BlockingQueuedConnection is the one case where blocking the
            # interface thread is right - the alternative is a timer that outlives its owner.
            QMetaObject.invokeMethod(
                self.worker, "shutdown", Qt.ConnectionType.BlockingQueuedConnection
            )
        if self.thread is not None:
            self.thread.quit()
            if not self.thread.wait(4000):
                # Better a warning on stderr than a destroyed-while-running thread, which
                # aborts the process on some platforms and hangs on others.
                print("the link thread did not stop within 4s", file=sys.stderr)
                self.thread.terminate()
                self.thread.wait(1000)
        if self.worker is not None:
            self.worker.deleteLater()
        if self.bridge is not None:
            self.bridge.stop()

    # ------------------------------------------------------------------ signals

    @pyqtSlot()
    def _on_connect_clicked(self) -> None:
        self.request_connect.emit()

    @pyqtSlot(str)
    def _on_unlock_clicked(self, code: str) -> None:
        self._log("unlocking")
        self.request_unlock.emit(code)

    @pyqtSlot()
    def _on_read_clicked(self) -> None:
        self.transfer.set_busy(True, "read")
        self.request_read.emit()

    @pyqtSlot()
    def _on_write_clicked(self) -> None:
        assert self.worker is not None
        changed = self.editor.changed_items()
        # Confirm before the irreversible step. Not for the write itself - that is
        # reversible - but because ACTIVATE changes what the engine will run, and the
        # operator should have seen the list of items in the same breath as the decision.
        names = ", ".join(f"{i.group}.{i.name}" for i in changed[:8])
        more = f"\n... and {len(changed) - 8} more" if len(changed) > 8 else ""
        answer = QMessageBox.question(
            self,
            "Write this tune?",
            f"{len(changed)} item(s) will be written, verified by the ECU, and activated:\n\n"
            f"{names}{more}",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            self._log("write cancelled by the operator")
            self.transfer.set_busy(False)
            return
        self.transfer.set_busy(True, "write")
        self._log(f"writing {len(changed)} changed item(s)")
        # The image goes over FIRST, as a separate queued event. Both are queued, so the
        # order they are emitted in is the order the worker sees them - and flashing first
        # would write the tune the worker last read, which is the previous one. That would
        # look like a successful flash of the wrong data.
        if self._image is not None:
            self.request_set_calibration.emit(self._image)
        self.request_write.emit()

    @pyqtSlot()
    def _on_rollback_clicked(self) -> None:
        answer = QMessageBox.question(
            self,
            "Roll back?",
            "The ECU will return to the tune that was active before the last write.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        self._log("rolling back")
        self.request_rollback.emit()

    @pyqtSlot(object)
    def _on_state(self, state) -> None:
        self.transfer.set_state(state)
        self.dashboard.set_link(state.connected, state.authenticated, state.engine_running)

    @pyqtSlot(object)
    def _on_connected(self, info) -> None:
        self._log(f"connected: firmware {info.firmware_text}, {info.item_count} items, "
                  f"{info.cylinder_count} cylinders")
        self.statusBar().showMessage(f"connected to {self.bridge.sim.port if self.bridge else ''}")

    @pyqtSlot()
    def _on_disconnected(self) -> None:
        self._log("disconnected")
        self.dashboard.clear()
        self.editor.load(cal.Calibration())
        self.transfer.set_busy(False)

    @pyqtSlot(object)
    def _on_live(self, live) -> None:
        self._last_frame_at = time.monotonic()
        self.dashboard.on_live(live, 0.0)

    @pyqtSlot(object)
    def _on_calibration(self, image) -> None:
        self._image = image
        # The editor snapshots the baseline itself, so the working copy and the ECU's tune
        # are two objects and a change is detectable.
        self.editor.load(image)
        self.transfer.set_calibration(image)
        self.transfer.set_changed([])
        self.transfer.set_busy(False)
        self.transfer.on_success(f"read {len(image.to_bytes())} bytes from the ECU")
        self._log(f"read {len(image.to_bytes())} bytes, "
                  f"cylinder_count {image.get('engine.cylinder_count'):g}")

    @pyqtSlot(int, int)
    def _on_reading(self, done: int, total: int) -> None:
        self.transfer.on_progress("read", done, total)

    @pyqtSlot(object)
    def _on_flashed(self, info) -> None:
        self.transfer.set_busy(False)
        self._log("tune written, verified by the ECU, and activated")
        # Re-read so the editor's baseline is the ECU's, which turns the diff back into
        # "nothing changed" - otherwise the panel still shows the write as outstanding.
        self._on_read_clicked()

    @pyqtSlot(str)
    def _on_message(self, text: str) -> None:
        self._log(text)
        self.transfer.on_message(text)

    @pyqtSlot(str)
    def _on_failed(self, text: str) -> None:
        self._log(text, error=True)
        self.transfer.on_error(text)
        self.transfer.set_busy(False)

    @pyqtSlot()
    def _on_unlock_prompt(self) -> None:
        self.tabs.setCurrentWidget(self.transfer)
        self.transfer.code.setFocus()

    @pyqtSlot(object)
    def _on_profile_built(self, image) -> None:
        """A profile-built calibration becomes the working image in the editor.

        Loaded with no baseline, so every value the profile derived shows up as a change
        against nothing - which is not useful. It is loaded as the image itself, and the diff
        is against the ECU's tune, so an operator can see at a glance whether the profile
        they just chose differs from what is on the motor.
        """
        self._image = image
        self.editor.load(image)
        self.transfer.set_calibration(image)
        self.transfer.set_changed([])
        self.tabs.setCurrentWidget(self.editor)
        self._log(f"profile calibration loaded into the editor ({len(image.to_bytes())} bytes)")

    @pyqtSlot()
    def _on_editor_changed(self) -> None:
        if self._image is not None:
            self.request_set_calibration.emit(self._image)
        self.transfer.set_changed(self.editor.changed_items())

    # --------------------------------------------------------------------- tick

    @pyqtSlot()
    def _on_tick(self) -> None:
        """Decide whether the display is still telling the truth.

        Called on the interface thread and never touches the link. If the last frame is
        older than the threshold, everything goes stale and says so - because a gauge that
        keeps showing the last value it received, unchanged, is indistinguishable from a
        steady engine, and that is the one mistake a dashboard must not make.
        """
        age = time.monotonic() - self._last_frame_at if self._last_frame_at else None
        if self._last_frame_at and age is not None and age > STALE_AFTER_S:
            self.dashboard.on_timeout()
            self.statusBar().showMessage(f"no live data for {age:.1f}s")
        elif self._last_frame_at:
            self.dashboard.freshness.setText(f"live {age:.2f}s ago")

    # ---------------------------------------------------------------------- log

    def _log(self, text: str, error: bool = False) -> None:
        stamp = time.strftime("%H:%M:%S")
        prefix = "!!" if error else "  "
        self.log.appendPlainText(f"{stamp} {prefix} {text}")
        self.log.moveCursor(QTextCursor.MoveOperation.End)
