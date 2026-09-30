"""The project screen: pick a bike, get a calibration.

This is the screen that answers "how do I configure this ECU to my motorcycle?", and it is
built around one decision: **nothing here can hide a guess.**

Choosing a profile shows three things side by side - what was derived, what is assumed, and
what has to be measured. The list of measurements is not advice in a tooltip; it is the
panel's own content, because it is the work that has to happen before the tune is finished,
and burying it is how a profile ends up on a track with a 36-tooth wheel nobody counted.

The build button is always available, and building always works: a project is a starting
point, not a finished tune, and refusing to build would be a worse answer than building and
saying plainly what is still wrong.
"""

from __future__ import annotations

from dataclasses import dataclass

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from raccoon import build, calibration as cal
from raccoon.vehicle import PROFILE_DIR, VehicleProfile, available_profiles

MEASURED = "#4ade80"
ASSUMED = "#fbbf24"
UNVERIFIED = "#f87171"
MUTED = "#94a3b8"


@dataclass
class Project:
    """A profile plus what it is being used for.

    The spec's project system names projects like GT650_TRACK and CUSTOM_4CYL: a vehicle
    profile combined with a purpose. The purpose is not cosmetic - a drag build runs a
    different limit strategy to a track build, and a profile marked `use` carries that.
    """

    name: str
    profile: VehicleProfile
    use: str = "road"
    notes: str = ""


class ProjectScreen(QWidget):
    """Choose a profile, read its honesty report, build a calibration."""

    calibration_built = pyqtSignal(object)     # cal.Calibration
    status = pyqtSignal(str)

    def __init__(self) -> None:
        super().__init__()
        self._profiles: dict[str, VehicleProfile] = {}
        self._image: cal.Calibration | None = None
        self._report: build.BuildReport | None = None

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)
        root.addWidget(self._build_selector())

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self._build_honesty())
        split.addWidget(self._build_outstanding())
        split.setStretchFactor(0, 1)
        split.setStretchFactor(1, 1)
        root.addWidget(split, stretch=1)
        root.addWidget(self._build_actions())

        self.reload()

    # ------------------------------------------------------------------ layout

    def _build_selector(self) -> QWidget:
        box = QGroupBox("vehicle")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)

        form = QFormLayout()
        self.profile_box = QComboBox()
        self.profile_box.currentIndexChanged.connect(self._on_profile_changed)
        form.addRow("engine profile", self.profile_box)

        self.use_box = QComboBox()
        for use in ("road", "track", "drag"):
            self.use_box.addItem(use)
        self.use_box.setToolTip(
            "The purpose. A track or drag project warns that the limits it widens have to be "
            "survivable by the hardware - a rev limit is a claim about the valves."
        )
        form.addRow("built for", self.use_box)

        self.name_edit = QComboBox()
        self.name_edit.setEditable(True)
        form.addRow("project name", self.name_edit)
        layout.addLayout(form)

        self.summary = QLabel("")
        self.summary.setWordWrap(True)
        self.summary.setStyleSheet(f"color:{MUTED};")
        layout.addWidget(self.summary)

        row = QHBoxLayout()
        load = QPushButton("Load a profile file...")
        load.clicked.connect(self._on_load_file)
        row.addWidget(load)
        row.addStretch(1)
        layout.addLayout(row)
        return box

    def _build_honesty(self) -> QWidget:
        box = QGroupBox("what this profile claims")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)

        self.honesty = QTableWidget(0, 3)
        self.honesty.setHorizontalHeaderLabels(["value", "provenance", "note"])
        header = self.honesty.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.honesty.verticalHeader().setVisible(False)
        self.honesty.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.honesty.setWordWrap(True)
        layout.addWidget(self.honesty)

        self.honesty_note = QLabel("")
        self.honesty_note.setWordWrap(True)
        self.honesty_note.setStyleSheet(f"color:{MUTED}; font-size:11px;")
        layout.addWidget(self.honesty_note)
        return box

    def _build_outstanding(self) -> QWidget:
        box = QGroupBox("what still has to be done")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)

        self.derived = QListWidget()
        self.derived.setToolTip(
            "Computed from the profile and from arithmetic. These are correct by construction."
        )
        # Word wrap, because the whole point of these lines is the sentence at the end of
        # them, and a horizontal scrollbar that hides the sentence defeats the panel.
        self.derived.setWordWrap(True)
        self.derived.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.derived.setTextElideMode(Qt.TextElideMode.ElideNone)
        layout.addWidget(QLabel("derived:"))
        layout.addWidget(self.derived, stretch=1)

        self.outstanding = QListWidget()
        self.outstanding.setToolTip(
            "Measurements. A profile cannot do these - they need the engine and an instrument."
        )
        self.outstanding.setWordWrap(True)
        self.outstanding.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.outstanding.setTextElideMode(Qt.TextElideMode.ElideNone)
        layout.addWidget(QLabel("to measure:"))
        layout.addWidget(self.outstanding, stretch=1)
        return box

    def _build_actions(self) -> QWidget:
        box = QGroupBox("build")
        layout = QHBoxLayout(box)
        layout.setContentsMargins(8, 8, 8, 8)

        self.build_button = QPushButton("Build calibration from this profile")
        self.build_button.setMinimumHeight(34)
        self.build_button.setFont(QFont("DejaVu Sans", 11, QFont.Weight.Bold))
        self.build_button.clicked.connect(self._on_build)
        layout.addWidget(self.build_button)

        self.result = QLabel("")
        self.result.setWordWrap(True)
        self.result.setStyleSheet(f"color:{MUTED};")
        layout.addWidget(self.result, stretch=1)
        return box

    # ------------------------------------------------------------------ loading

    def reload(self) -> None:
        """Re-read the profile directory.

        Called at startup and after an external edit, because a profile is a text file and
        somebody will edit it in an editor rather than through this screen.
        """
        self._profiles = {}
        self.profile_box.blockSignals(True)
        self.profile_box.clear()
        for path in available_profiles():
            try:
                profile = VehicleProfile.load(path)
            except Exception as exc:      # noqa: BLE001 - a bad file must not kill the screen
                self.status.emit(f"{path.name} did not load: {exc}")
                continue
            self._profiles[profile.name] = profile
            self.profile_box.addItem(profile.name)
        self.profile_box.blockSignals(False)
        self.name_edit.clear()
        self._on_profile_changed()

    def _on_load_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Load a vehicle profile", str(PROFILE_DIR), "Profiles (*.json)")
        if not path:
            return
        try:
            profile = VehicleProfile.load(path)
        except Exception as exc:          # noqa: BLE001
            self.status.emit(f"that profile did not load: {exc}")
            return
        self._profiles[profile.name] = profile
        index = self.profile_box.findText(profile.name)
        if index < 0:
            self.profile_box.addItem(profile.name)
            index = self.profile_box.count() - 1
        self.profile_box.setCurrentIndex(index)

    def _on_profile_changed(self) -> None:
        profile = self._profiles.get(self.profile_box.currentText())
        self.build_button.setEnabled(profile is not None)
        if profile is None:
            return
        self.summary.setText(profile.summary() + "\n" + profile.description)
        if not self.name_edit.currentText():
            self.name_edit.setItemText(0, f"{profile.name.upper()}_"
                                        f"{self.use_box.currentText().upper()}")
        self._fill_honesty(profile)
        self.derived.clear()
        self.outstanding.clear()
        self.result.setText("")
        self.build_button.setText(f"Build calibration from {profile.name}")

    def _fill_honesty(self, profile: VehicleProfile) -> None:
        """Every value, with its provenance. Not just the unverified ones.

        A table of only the guesses is a list of problems. A table of everything puts each
        guess in its place, so the reader can see that the bore is from a datasheet and the
        trigger count is not - which is the distinction that matters when deciding what to
        check first.
        """
        rows: list[tuple[str, str, str, str]] = []
        for section_name in ("geometry", "trigger", "hardware", "limits"):
            section = getattr(profile, section_name)
            for name, value in vars(section).items():
                # Only the wrapped values; firing_order is a plain list and has no
                # provenance of its own, so it is reported through the geometry it belongs to.
                if not hasattr(value, "provenance"):
                    continue
                rows.append((f"{section_name}.{name}", str(value.value),
                             value.provenance.value, value.note))
        rows.sort(key=lambda r: (r[2], r[0]))
        self.honesty.setRowCount(len(rows))
        for i, (name, number, provenance, note) in enumerate(rows):
            self.honesty.setItem(i, 0, QTableWidgetItem(f"{name} = {number}"))
            tag = QTableWidgetItem(provenance)
            colour = {MEASURED: MEASURED, "assumed": ASSUMED}.get(provenance, UNVERIFIED)
            tag.setForeground(QColor(colour))
            self.honesty.setItem(i, 1, tag)
            self.honesty.setItem(i, 2, QTableWidgetItem(note))
        outstanding = len(profile.unverified())
        total = len(rows)
        self.honesty_note.setText(
            f"{total - outstanding} of {total} values are measured, from a datasheet, or "
            f"established by wiring. {outstanding} are not, and the notes say why."
        )

    # ------------------------------------------------------------------ build

    def _on_build(self) -> None:
        profile = self._profiles.get(self.profile_box.currentText())
        if profile is None:
            return
        use = self.use_box.currentText()
        name = self.name_edit.currentText().strip() or f"{profile.name}_{use}"
        project = Project(name=name, profile=profile, use=use)

        try:
            image, report = build.build(profile)
        except build.ProfileError as exc:
            # A refusal is a result, not a crash. The operator's next action is to fix the
            # profile, and the message tells them which field.
            self.result.setText(str(exc))
            self.result.setStyleSheet(f"color:{UNVERIFIED};")
            self.status.emit(f"{name}: refused, {len(str(exc).splitlines()) - 1} problem(s)")
            return

        self._image = image
        self._report = report
        self.derived.clear()
        self.derived.addItems(report.derived)
        self.outstanding.clear()
        self.outstanding.addItems(report.outstanding)

        self.result.setText(
            f"{name}: {len(report.derived)} values derived, "
            f"{len(report.outstanding)} measurements outstanding, "
            f"{len(report.warnings)} warning(s)."
        )
        self.result.setStyleSheet(f"color:{ASSUMED if report.outstanding else MEASURED};")
        self.status.emit(f"{name}: calibration built, {len(report.outstanding)} item(s) to measure")
        self.calibration_built.emit(image)
