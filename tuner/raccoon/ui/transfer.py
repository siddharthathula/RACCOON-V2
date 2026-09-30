"""The transfer panel: what is about to change, and the button that does it.

The whole screen is arranged around one question an operator has to answer before pressing
anything: **what exactly is this about to do to the ECU?**

So the panel shows the item count, the running status, the reason a write is refused if it
is, and - once a read has happened - the diff. The write button stays disabled until there
is something to write, and the reason it is disabled is on the button's own row rather than
in a dialog the operator has to trigger to discover.

Nothing here activates a tune directly. Writing is a three-step operation the ECU performs
itself, and the panel reports each step as it happens rather than showing one progress bar
for a process whose failure leaves the ECU in a state the operator needs to know about.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from raccoon import calibration as cal
from raccoon.ui.worker import ConnectionState

OK = QColor("#4ade80")
WARN = QColor("#fbbf24")
BAD = QColor("#f87171")
MUTED = QColor("#94a3b8")


class TransferPanel(QWidget):
    """Connect, unlock, read, write."""

    connect_requested = pyqtSignal()
    disconnect_requested = pyqtSignal()
    unlock_requested = pyqtSignal(str)
    read_requested = pyqtSignal()
    write_requested = pyqtSignal()
    rollback_requested = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        self._state = ConnectionState()
        self._changed: list = []
        self._busy = False

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)
        root.addWidget(self._build_connection())
        root.addWidget(self._build_write())
        root.addWidget(self._build_progress())
        root.addStretch(1)
        self._refresh()

    # -------------------------------------------------------------- connection

    def _build_connection(self) -> QWidget:
        box = QGroupBox("link")
        form = QFormLayout(box)
        form.setContentsMargins(8, 8, 8, 8)
        form.setSpacing(6)

        self.status = QLabel("not connected")
        self.status.setFont(QFont("DejaVu Sans", 12, QFont.Weight.Bold))
        self.status.setStyleSheet("color:#94a3b8;")
        form.addRow("status", self.status)

        self.identity = QLabel("-")
        self.identity.setStyleSheet("color:#94a3b8; font-size:11px;")
        form.addRow("ecu", self.identity)

        self.refusal = QLabel("")
        self.refusal.setWordWrap(True)
        self.refusal.setStyleSheet("color:#fbbf24;")
        form.addRow("", self.refusal)

        row = QHBoxLayout()
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self._on_connect)
        row.addWidget(self.connect_button)

        self.disconnect_button = QPushButton("Disconnect")
        self.disconnect_button.clicked.connect(self.disconnect_requested.emit)
        row.addWidget(self.disconnect_button)
        row.addStretch(1)
        form.addRow("", row)

        row2 = QHBoxLayout()
        self.code = QLineEdit()
        self.code.setEchoMode(QLineEdit.EchoMode.Password)
        self.code.setPlaceholderText("unlock code")
        self.code.returnPressed.connect(self._on_unlock)
        row2.addWidget(self.code)
        self.unlock_button = QPushButton("Unlock")
        self.unlock_button.clicked.connect(self._on_unlock)
        row2.addWidget(self.unlock_button)
        form.addRow("unlock", row2)

        read_row = QHBoxLayout()
        self.read_button = QPushButton("Read tune from ECU")
        self.read_button.clicked.connect(self.read_requested.emit)
        read_row.addWidget(self.read_button)
        self.rollback_button = QPushButton("Roll back")
        self.rollback_button.setToolTip(
            "Return to the tune that was active before the last write. Refused if the other "
            "slot holds no valid calibration."
        )
        self.rollback_button.clicked.connect(self.rollback_requested.emit)
        read_row.addWidget(self.rollback_button)
        read_row.addStretch(1)
        form.addRow("", read_row)

        return box

    # -------------------------------------------------------------------- write

    def _build_write(self) -> QWidget:
        box = QGroupBox("write")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        self.summary = QLabel("no tune loaded")
        self.summary.setWordWrap(True)
        self.summary.setStyleSheet("color:#94a3b8;")
        layout.addWidget(self.summary)

        self.diff = QLabel("")
        self.diff.setWordWrap(True)
        layout.addWidget(self.diff)

        self.write_button = QPushButton("Write tune to ECU")
        self.write_button.setMinimumHeight(38)
        self.write_button.setFont(QFont("DejaVu Sans", 12, QFont.Weight.Bold))
        self.write_button.clicked.connect(self.write_requested.emit)
        layout.addWidget(self.write_button)

        # The sequence, spelled out. A single "write" button hides the fact that the ECU
        # stages, verifies and activates separately, and an operator watching a progress bar
        # has no idea which of those is happening when it fails.
        steps = QLabel(
            "The ECU writes to the inactive slot, reads it back and checks it, and only "
            "then swaps. A failure at any step leaves the running tune untouched."
        )
        steps.setWordWrap(True)
        steps.setStyleSheet("color:#64748b; font-size:11px;")
        layout.addWidget(steps)
        return box

    # ----------------------------------------------------------------- progress

    def _build_progress(self) -> QWidget:
        box = QGroupBox("progress")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)

        self.stage_label = QLabel("")
        self.stage_label.setStyleSheet("color:#94a3b8;")
        layout.addWidget(self.stage_label)

        self.bar = QProgressBar()
        self.bar.setRange(0, 100)
        self.bar.setValue(0)
        layout.addWidget(self.bar)
        return box

    # ------------------------------------------------------------------ updates

    def _on_connect(self) -> None:
        # Connect is a toggle, so one button does both and can never be in a state where it
        # says "connect" while already connected.
        if self._state.connected:
            self.disconnect_requested.emit()
        else:
            self.connect_requested.emit()

    def _on_unlock(self) -> None:
        code = self.code.text()
        if not code:
            return
        self.unlock_requested.emit(code)

    def set_state(self, state: ConnectionState) -> None:
        self._state = state
        self._refresh()

    def set_calibration(self, image: cal.Calibration | None) -> None:
        self._image = image
        self._refresh()

    def set_changed(self, items: list) -> None:
        self._changed = items
        self._refresh()

    def set_busy(self, busy: bool, what: str = "") -> None:
        self._busy = busy
        self.bar.setVisible(busy or bool(what))
        if not busy and not what:
            self.stage_label.setText("")
        self._refresh()

    def on_progress(self, stage: str, done: int, total: int) -> None:
        if total > 0:
            self.bar.setRange(0, total)
            self.bar.setValue(done)
        text = {"stage": "sending", "activate": "activating"}.get(stage, stage)
        self.stage_label.setText(f"{text}  {done}/{total} bytes")

    def on_message(self, text: str) -> None:
        self.stage_label.setText(text)
        self.stage_label.setStyleSheet("color:#94a3b8;")

    def on_error(self, text: str) -> None:
        self.stage_label.setText(text.splitlines()[0])
        self.stage_label.setStyleSheet("color:#f87171;")

    def on_success(self, text: str) -> None:
        self.bar.setValue(self.bar.maximum())
        self.stage_label.setText(text)
        self.stage_label.setStyleSheet("color:#4ade80;")

    def _refresh(self) -> None:
        state = self._state
        connected = state.connected
        authenticated = state.authenticated

        if not connected:
            self.status.setText("not connected")
            self.status.setStyleSheet("color:#94a3b8;")
            self.identity.setText("-")
        else:
            info = state.info
            self.status.setText("connected" + ("  -  unlocked" if authenticated else "  -  LOCKED"))
            self.status.setStyleSheet("color:" + (OK.name() if authenticated else WARN.name()))
            if info is not None:
                flags = []
                if info.armed:
                    flags.append("engine running")
                if info.flash_rolled_back:
                    flags.append("FLASH WAS ROLLED BACK")
                self.identity.setText(
                    f"firmware {info.firmware_text}  -  {info.item_count} items  -  "
                    f"{info.cylinder_count} cyl  -  slot {info.flash_active_slot} "
                    f"v{info.flash_active_version}"
                    + ("  -  " + ", ".join(flags) if flags else "")
                )

        self.connect_button.setText("Disconnect" if connected else "Connect")
        self.disconnect_button.setEnabled(connected)
        self.unlock_button.setEnabled(connected and not authenticated and not self._busy)
        self.code.setEnabled(connected and not authenticated and not self._busy)
        self.read_button.setEnabled(connected and not self._busy)
        self.rollback_button.setEnabled(bool(connected and authenticated and not self._busy))

        refused = state.reason_writes_are_refused()
        self.refusal.setText(f"writes are refused: {refused}" if refused else "")

        image = getattr(self, "_image", None)
        changed = self._changed
        if image is None:
            self.summary.setText("no tune loaded - read one from the ECU to begin")
        elif not changed:
            self.summary.setText("the tune in the editor matches the ECU's")
        else:
            self.summary.setText(
                f"{len(changed)} item(s) differ from the ECU's tune and will be written"
            )
            names = []
            for item in changed[:6]:
                names.append(f"{item.group}.{item.name}")
            more = f"  (+{len(changed) - 6} more)" if len(changed) > 6 else ""
            self.diff.setText(", ".join(names) + more)
            self.diff.setStyleSheet("color:#fbbf24;")
        if image is None or not changed:
            self.diff.setText("")

        can_write = bool(connected and authenticated and changed and not refused and not self._busy)
        self.write_button.setEnabled(can_write)
        if not can_write:
            if refused:
                self.write_button.setText("Write tune to ECU")
            elif not connected:
                self.write_button.setText("Write tune to ECU")
            elif not changed:
                self.write_button.setText("Nothing to write")
            else:
                self.write_button.setText("Write tune to ECU")
        else:
            self.write_button.setText(f"Write tune to ECU  ({len(changed)} items)")
