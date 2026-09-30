"""Shared state and the worker thread that owns the link.

One design decision shapes this whole package, and it is worth stating before any widget
exists: **the link is owned by one thread, and every other thread talks to it through
signals.**

:mod:`raccoon.client` is explicitly not safe for concurrent use - it pairs replies with
requests by a single sequence number, and two threads sending at once produce a log full of
values from the wrong moment, which is close to undebuggable from the symptom. So the link
runs on a :class:`QThread` and widgets never touch it directly. They post a request and
receive a result.

The alternative - a lock - is worse here. A blocking read inside a Qt slot freezes the whole
interface, and "the window stopped responding" is indistinguishable from a crashed ECU.
Everything slow happens on the worker, and the interface stays honest.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from PyQt6.QtCore import QObject, QThread, QTimer, pyqtSignal, pyqtSlot

from raccoon import calibration as cal
from raccoon.client import ClientError, EcuClient, EcuInfo, NotAuthenticated, ResultError
from raccoon.link import LiveData, safety_text
from raccoon.transport import Transport, TransportError

# How often the dashboard is repainted. Fast enough to look live, slow enough that the GUI
# thread is never the bottleneck - the ECU's service rate is 2 kHz and a repaint at 50 Hz
# shows every change a person can perceive.
REPAINT_HZ = 20


@dataclass
class ConnectionState:
    """Everything the interface needs to know about the link, in one immutable-ish bundle.

    A single object rather than a dozen booleans on the window, because the states are not
    independent: "unlocked" is meaningless unless "connected", and a set of independent flags
    eventually represents a combination that cannot happen - and then the interface offers a
    button that the ECU will refuse.
    """

    connected: bool = False
    authenticated: bool = False
    info: EcuInfo | None = None
    message: str = ""
    error: str = ""

    @property
    def engine_running(self) -> bool:
        return bool(self.info and self.info.armed)

    def reason_writes_are_refused(self) -> str:
        """Why a write would be refused, or "" if it would not be.

        Returned as a sentence rather than a flag so the button can say WHY it is disabled.
        A greyed-out control with no explanation is the thing an operator works around.
        """
        if not self.connected:
            return "not connected to an ECU"
        if not self.authenticated:
            return "locked - enter the unlock code"
        if self.engine_running:
            return "the engine is running - stop it before changing a tune"
        return ""


class LinkWorker(QObject):
    """Runs the link on its own thread.

    Signals, rather than callbacks into widgets, so the interface thread is never blocked by
    a transfer and a slow ECU cannot freeze the window.
    """

    connected = pyqtSignal(object)          # EcuInfo
    disconnected = pyqtSignal()
    state_changed = pyqtSignal(object)      # ConnectionState
    live = pyqtSignal(object)               # LiveData
    # Named `calibration_loaded`, not `calibration`: a property and a signal of the same
    # name in one class is a collision Qt does not report. The later definition silently
    # wins, and `worker.calibration.connect(...)` then fails with a NoneType error pointing
    # at a line that looks perfectly correct.
    calibration_loaded = pyqtSignal(object)  # cal.Calibration
    calibration_reading = pyqtSignal(int, int)
    flashing = pyqtSignal(str, int, int)    # stage, done, total
    flashed = pyqtSignal(object)            # EcuInfo
    message = pyqtSignal(str)
    failed = pyqtSignal(str)
    unlock_prompt = pyqtSignal()

    def __init__(self, transport: Transport, unlock_code: str = "") -> None:
        super().__init__()
        self.transport = transport
        self.unlock_code = unlock_code
        self.client = EcuClient(transport)
        self.state = ConnectionState()
        self._stop = False
        self._timer: QTimer | None = None
        self._calibration: cal.Calibration | None = None
        self._code: str = unlock_code

    # ------------------------------------------------------------- thread entry

    def run(self) -> None:  # pragma: no cover - runs on the QThread
        """Start polling and return immediately.

        A timer rather than a ``while`` loop. A loop here looks simpler and breaks shutdown:
        it never returns, so the thread's event loop never quits, ``QThread.wait`` times out,
        and closing the window either hangs or destroys a running thread - which on some
        platforms takes the process with it. A timer on the worker's own thread keeps the
        work off the interface thread AND lets the thread stop cleanly.
        """
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll_once)
        self._timer.start(int(1000 / REPAINT_HZ))

    @pyqtSlot()
    def shutdown(self) -> None:
        """Stop polling. Safe to call from the interface thread."""
        self._stop = True
        if getattr(self, "_timer", None) is not None:
            self._timer.stop()
        try:
            self.client.disconnect()
        except Exception:  # noqa: BLE001 - shutdown must not raise
            pass

    def _poll_once(self) -> None:
        if not self.state.connected:
            return
        frame = self.client.poll_live(0.0)
        if frame is None:
            return          # a stale gauge beats an error dialog every 50 ms
        self.live.emit(frame)
        # The armed bit can change without the operator doing anything to the tuner - a
        # start, a stall, a stall detection - and the write buttons depend on it. Derived
        # from the live frame rather than only from a reply, so they are never stale.
        was_running = self.state.engine_running
        if self.state.info is not None and bool(frame.armed) != was_running:
            self.refresh_info()

    @pyqtSlot()
    def stop(self) -> None:
        self._stop = True

    # --------------------------------------------------------------- operations

    @pyqtSlot()
    def do_connect(self) -> None:
        try:
            info = self.client.connect()
        except (TransportError, ClientError) as exc:
            self.state.connected = False
            self.state.error = str(exc)
            self.state_changed.emit(self.state)
            self.failed.emit(str(exc))
            return
        self.state.connected = True
        self.state.info = info
        self.state.error = ""
        self.connected.emit(info)
        self.state_changed.emit(self.state)
        self.message.emit(
            f"Connected: firmware {info.firmware_text}, {info.item_count} items, "
            f"{info.image_size} byte calibration"
        )
        # Logging is on by default. A dashboard that needs enabling before it shows anything
        # looks broken, and the operator's first conclusion is that the link is not working.
        try:
            self.client.set_logging(REPAINT_HZ)
        except (TransportError, ClientError):
            pass
        self.do_read_calibration()

    @pyqtSlot()
    def do_disconnect(self) -> None:
        self.client.disconnect()
        self.state.connected = False
        self.state.authenticated = False
        self.state.info = None
        self._calibration = None
        self.state_changed.emit(self.state)
        self.disconnected.emit()

    @pyqtSlot(str)
    def do_unlock(self, code: str) -> None:
        self._code = code
        try:
            self.client.authenticate(code)
        except (TransportError, ClientError) as exc:
            self.state.authenticated = False
            self.state.error = str(exc)
            self.state_changed.emit(self.state)
            self.failed.emit(str(exc))
            return
        self.state.authenticated = True
        self.state.error = ""
        self.state_changed.emit(self.state)
        self.message.emit("Unlocked. The ECU will accept a tune change now.")

    @pyqtSlot()
    def do_lock(self) -> None:
        # There is no "lock" command: the ECU holds no session state it would honour, and
        # the honest equivalent is to drop the link, which also re-locks the ECU's side.
        self.message.emit("Re-lock by disconnecting; the ECU locks on every new session.")

    @pyqtSlot()
    def do_refresh_info(self) -> None:
        self.refresh_info()

    def refresh_info(self) -> None:
        try:
            info = self.client.refresh_info()
        except (TransportError, ClientError):
            return
        self.state.info = info
        self.state_changed.emit(self.state)

    @pyqtSlot()
    def do_read_calibration(self) -> None:
        if not self.state.connected:
            self.failed.emit("connect to an ECU first")
            return
        self.message.emit("Reading calibration...")

        def progress(done: int, total: int) -> None:
            self.calibration_reading.emit(done, total)

        try:
            image = self.client.read_bulk(on_progress=progress)
        except (TransportError, ClientError) as exc:
            self.failed.emit(str(exc))
            return
        self._calibration = image
        self.calibration_loaded.emit(image)
        self.message.emit(f"Read {len(image.to_bytes())} bytes from the ECU.")

    @pyqtSlot()
    def do_flash(self) -> None:
        if self._calibration is None:
            self.failed.emit("read a calibration before writing one")
            return
        refused = self.state.reason_writes_are_refused()
        if refused:
            self.failed.emit(f"Cannot write a tune: {refused}.")
            return
        self.message.emit("Sending the tune...")

        # The client reports (done, total); the interface wants to know WHICH step it is.
        # Adapted here rather than by changing the client, because the client has no concept
        # of stages - it stages bytes - and giving it one would put a presentation concern in
        # the protocol layer.
        def progress(done: int, total: int) -> None:
            self.flashing.emit("stage", done, total)

        try:
            self.client.flash_calibration(self._calibration, self._code, on_progress=progress)
        except NotAuthenticated:
            self.state.authenticated = False
            self.state_changed.emit(self.state)
            self.failed.emit("The ECU refused the write because it is locked. Unlock again.")
            self.unlock_prompt.emit()
            return
        except (TransportError, ClientError) as exc:
            # The running tune is untouched, and that is worth saying: a failed write is not
            # a damaged engine, and an operator who thinks otherwise will not try again.
            self.failed.emit(f"{exc}\n\nThe ECU is still running the tune it had.")
            return
        self.flashing.emit("activate", 1, 1)
        info = self.state.info
        self.flashed.emit(info)
        self.message.emit("Tune written, verified by the ECU, and activated.")
        self.refresh_info()

    @pyqtSlot()
    def do_rollback(self) -> None:
        refused = self.state.reason_writes_are_refused()
        if refused:
            self.failed.emit(f"Cannot roll back: {refused}.")
            return
        try:
            self.client.rollback()
        except (TransportError, ClientError) as exc:
            self.failed.emit(str(exc))
            return
        self.message.emit("Rolled back to the previous tune.")
        self.do_read_calibration()

    # ------------------------------------------------------------- live plumbing

    @property
    def calibration(self) -> cal.Calibration | None:
        return self._calibration

    def set_calibration(self, image: cal.Calibration) -> None:
        self._calibration = image


class SafetySummary:
    """Turns a safety bitmask into something a person can act on.

    The mask is 32 bits of which a few are set, and a tuner that shows the raw number is
    showing a puzzle rather than a warning. The mapping lives with the link protocol so the
    names and the bits cannot drift apart.
    """

    LABELS = {
        0x0001: "overspeed",
        0x0002: "lean",
        0x0004: "rich",
        0x0008: "knock",
        0x0010: "oil pressure",
        0x0020: "oil temperature",
        0x0040: "coolant temperature",
        0x0080: "battery",
        0x0100: "trigger sync lost",
        0x0200: "cranking only",
        0x0400: "sensor fault",
        0x8000: "launch active",
    }

    @classmethod
    def active(cls, mask: int) -> list[str]:
        return [name for bit, name in sorted(cls.LABELS.items()) if mask & bit]
