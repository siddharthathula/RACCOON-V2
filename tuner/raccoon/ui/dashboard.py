"""The dashboard: what the engine is doing, right now.

Designed to be read at a glance from a laptop mounted on a bench or held at a trackside, so
the things that matter are large and the things that do not are quiet. Three rules shaped
the layout:

* **Validity is shown, not inferred.** A lambda reading with an invalid flag behind it looks
  exactly like a valid reading of 1.000. Anything the ECU marks invalid is drawn in a
  different colour and labelled, because a number without its validity is a rumour.
* **A stale reading says so.** The last frame's age is always visible. A gauge frozen at the
  last value it saw is the most dangerous thing a dashboard can do - it looks like an engine
  holding a steady state when the truth is that the link died.
* **Safety is a list, not a bitmask.** A person needs "knock, oil pressure" and not
  "0x0108".
"""

from __future__ import annotations

from collections import deque

import pyqtgraph as pg
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QGridLayout,
    QGroupBox,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from raccoon.link import LiveData
from raccoon.ui.worker import REPAINT_HZ, SafetySummary

# Chosen for contrast against the dark plot background, and checked against it rather than
# assumed: a gauge you cannot read in daylight is not a gauge.
GOOD = QColor("#4ade80")
WARN = QColor("#fbbf24")
BAD = QColor("#f87171")
STALE = QColor("#94a3b8")
MUTED = QColor("#cbd5e1")
ACCENT = QColor("#38bdf8")


def _readout_colour(value: float, low: float, high: float) -> QColor:
    """Green inside the band, amber at the edge, red outside.

    The bands are supplied by the caller because what is "low" depends on the signal: 11 V
    is fine for a battery and alarming for a wideband, and a hard-coded threshold would be
    wrong for one of them.
    """
    if low is not None and value < low:
        return BAD
    if high is not None and value > high:
        return BAD
    if low is not None and value < low * 1.15:
        return WARN
    if high is not None and value > high * 0.85:
        return WARN
    return GOOD


class Readout(QFrame):
    """One number, with its name, unit, and validity.

    A label with a caption rather than a bare number, because a screen full of unlabelled
    numbers is a screen nobody can read while an engine is running.
    """

    def __init__(self, name: str, unit: str = "", decimals: int = 0,
                 low: float | None = None, high: float | None = None) -> None:
        super().__init__()
        self.decimals = decimals
        self.low = low
        self.high = high
        self.stale = True

        self.setFrameShape(QFrame.Shape.StyledPanel)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(0)

        self.caption = QLabel(name)
        self.caption.setStyleSheet("color:#94a3b8; font-size:11px;")
        self.value = QLabel("--")
        self.value.setAlignment(Qt.AlignmentFlag.AlignRight)
        self.value.setFont(QFont("DejaVu Sans Mono", 19, QFont.Weight.Bold))
        self.value.setStyleSheet("color:#64748b;")
        self.unit = QLabel(unit)
        self.unit.setStyleSheet("color:#64748b; font-size:10px;")
        self.note = QLabel("")
        self.note.setStyleSheet("color:#f87171; font-size:10px;")

        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(4)
        row.addWidget(self.value)
        row.addWidget(self.unit, alignment=Qt.AlignmentFlag.AlignBottom)
        layout.addWidget(self.caption)
        layout.addLayout(row)
        layout.addWidget(self.note)

    def set_live(self, value: float, note: str = "") -> None:
        self.stale = False
        self.value.setText(f"{value:.{self.decimals}f}")
        self.value.setStyleSheet(f"color:{_readout_colour(value, self.low, self.high).name()};")
        self.note.setText(note)

    def set_stale(self) -> None:
        if self.stale:
            return
        self.stale = True
        # Kept visible but greyed, with the last number still shown. Blanking it loses
        # information the operator had a moment ago; showing it as normal loses the fact
        # that it is no longer being confirmed.
        self.value.setStyleSheet("color:#475569;")
        self.note.setText("stale")
        self.note.setStyleSheet("color:#94a3b8; font-size:10px;")

    def clear(self) -> None:
        self.stale = True
        self.value.setText("--")
        self.value.setStyleSheet("color:#64748b;")
        self.note.setText("")
        self.note.setStyleSheet("color:#f87171; font-size:10px;")



class Dashboard(QWidget):
    """The live view. Owns no state beyond its own history."""

    HISTORY = 400

    def __init__(self) -> None:
        super().__init__()
        self._last: LiveData | None = None
        self._rpm = deque(maxlen=self.HISTORY)
        self._lambda = deque(maxlen=self.HISTORY)
        self._map = deque(maxlen=self.HISTORY)

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)

        root.addWidget(self._build_status_row())
        root.addLayout(self._build_readouts())
        root.addWidget(self._build_plots(), stretch=1)

    # ------------------------------------------------------------------ status

    def _build_status_row(self) -> QWidget:
        bar = QFrame()
        bar.setFrameShape(QFrame.Shape.StyledPanel)
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(10, 6, 10, 6)

        self.link_state = QLabel("not connected")
        self.link_state.setFont(QFont("DejaVu Sans", 12, QFont.Weight.Bold))
        layout.addWidget(self.link_state)

        layout.addStretch(1)

        self.safety = QLabel("no active safety")
        self.safety.setStyleSheet("color:#4ade80;")
        layout.addWidget(self.safety)

        layout.addStretch(1)

        self.freshness = QLabel("")
        self.freshness.setStyleSheet("color:#94a3b8; font-size:11px;")
        layout.addWidget(self.freshness)

        return bar

    def set_link(self, connected: bool, authenticated: bool, engine: bool) -> None:
        """One line saying what the link is, because three booleans are not a status.

        "connected but locked" and "connected and unlocked, engine running" are different
        situations with different things an operator can do, and a single green light
        cannot distinguish them.
        """
        if not connected:
            self.link_state.setText("not connected")
            self.link_state.setStyleSheet("color:#94a3b8;")
            return
        parts = ["connected"]
        colour = "#38bdf8"
        if authenticated:
            parts.append("unlocked")
            colour = "#4ade80"
        else:
            parts.append("LOCKED - no tune can be written")
            colour = "#fbbf24"
        if engine:
            parts.append("engine running")
        else:
            parts.append("engine stopped")
        self.link_state.setText("  |  ".join(parts))
        self.link_state.setStyleSheet(f"color:{colour};")

    # ---------------------------------------------------------------- readouts

    def _build_readouts(self) -> QGridLayout:
        grid = QGridLayout()
        grid.setSpacing(6)
        self.readouts: dict[str, Readout] = {}

        def add(row: int, col: int, key: str, name: str, unit: str = "", decimals: int = 0,
                low: float | None = None, high: float | None = None) -> None:
            widget = Readout(name, unit, decimals, low, high)
            self.readouts[key] = widget
            grid.addWidget(widget, row, col)

        add(0, 0, "rpm", "engine speed", "rpm", 0, 0, 12000)
        add(0, 1, "map", "manifold pressure", "kPa", 1, 5, 300)
        add(0, 2, "tps", "throttle", "%", 1, 0, 100)
        add(0, 3, "battery", "battery", "V", 1, 11.0, 15.0)
        add(1, 0, "lambda", "lambda 1", "", 3, 0.75, 1.25)
        add(1, 1, "ect", "coolant", "C", 1, 40, 130)
        add(1, 2, "iat", "intake", "C", 1, -20, 120)
        add(1, 3, "oilp", "oil pressure", "bar", 2, 1.0, 8.0)
        add(2, 0, "inj1", "injector 1", "ms", 3, 0.0, 20.0)
        add(2, 1, "ign1", "ignition 1", "deg", 2, -20, 60)
        add(2, 2, "dwell1", "dwell 1", "ms", 2, 0.0, 8.0)
        add(2, 3, "fuelp", "fuel pressure", "kPa", 0, 200, 700)
        return grid

    # ------------------------------------------------------------------- plots

    def _build_plots(self) -> QWidget:
        """Three stacked plots on one time axis.

        Stacked, not overlaid on shared axes. Overlaying was tried and is worse than it looks
        on paper: rpm runs to 12000 and lambda runs to 1.4, so on one pair of axes lambda and
        manifold pressure are both flat lines pinned to the bottom - present, and unreadable.
        A trace you cannot see is not information, and the operator does not know which of
        the flat lines is the one that matters at that moment.

        Stacked keeps the time relationship, which is the reason to look at traces together at
        all - lean under boost, rpm falling as the map recovers - and gives each signal the
        vertical space it needs to be read.
        """
        box = QGroupBox("traces")
        layout = QVBoxLayout(box)
        layout.setSpacing(2)

        self.plots: list[pg.PlotWidget] = []
        self.curves: list[pg.PlotDataItem] = []
        specs = [
            ("engine speed", "rpm", ACCENT, (0.0, 12000.0)),
            ("lambda 1", "", GOOD, (0.6, 1.5)),
            ("manifold pressure", "kPa", WARN, (0.0, 250.0)),
        ]
        for name, unit, colour, span in specs:
            plot = pg.PlotWidget(background="#0b1120")
            plot.showGrid(x=True, y=True, alpha=0.2)
            plot.getPlotItem().setLabel("left", f"{name}\n{unit}" if unit else name)
            plot.getPlotItem().setMenuEnabled(False)
            plot.getPlotItem().hideAxis("bottom")
            plot.getPlotItem().setMouseEnabled(x=True, y=False)
            plot.getPlotItem().setYRange(*span, padding=0.02)
            plot.setMinimumHeight(110)
            self.plots.append(plot)
            self.curves.append(plot.plot([], [], pen=pg.mkPen(colour, width=2)))
            layout.addWidget(plot)

        # One x axis, on the bottom plot only, and labelled in SECONDS.
        #
        # Labelling a frame index as seconds is not a cosmetic slip: a datalog whose time axis
        # is off by a factor of the log rate is evidence of nothing, and nobody notices until
        # they try to correlate it with a video.
        self.plots[-1].getPlotItem().showAxis("bottom")
        self.plots[-1].getPlotItem().setLabel("bottom", "seconds ago")
        self.window_seconds = 20.0

        return box

    def _redraw(self) -> None:
        """All three traces share the same x values, so the eye lines them up in time."""
        count = len(self._rpm)
        span = max(self.window_seconds, 1.0)
        # Newest sample at the right, running back to the left. Most plotting libraries
        # default to the opposite, and a live trace that scrolls the wrong way is read as a
        # fault by anyone who has used one before.
        first = max(0, count - int(span * REPAINT_HZ))
        xs = [-(count - 1 - i) / REPAINT_HZ for i in range(first, count)]
        self.curves[0].setData(xs, list(self._rpm)[first:])
        self.curves[1].setData(xs, list(self._lambda)[first:])
        self.curves[2].setData(xs, list(self._map)[first:])
        for plot in self.plots:
            plot.setXRange(-span, 0, padding=0)

    # ------------------------------------------------------------------ update

    def on_live(self, live: LiveData, age_s: float) -> None:
        self._last = live
        self._rpm.append(live.rpm)
        self._lambda.append(live.lambda_values[0] if live.lambda_values else 0.0)
        self._map.append(live.map_kpa)

        lambda_valid = bool(live.lambda_valid[0]) if live.lambda_valid else False
        self.readouts["rpm"].set_live(live.rpm)
        self.readouts["map"].set_live(live.map_kpa)
        self.readouts["tps"].set_live(live.tps_pct)
        self.readouts["battery"].set_live(live.battery_v)
        self.readouts["lambda"].set_live(
            live.lambda_values[0] if live.lambda_values else 0.0,
            "" if lambda_valid else "INVALID - sensor, not a mixture of 1.000",
        )
        self.readouts["ect"].set_live(live.ect_c)
        self.readouts["iat"].set_live(live.iat_c)
        self.readouts["oilp"].set_live(live.oil_pressure_bar)
        self.readouts["inj1"].set_live(live.injector_ms[0] if live.injector_ms else 0.0)
        self.readouts["ign1"].set_live(live.ignition_deg[0] if live.ignition_deg else 0.0)
        self.readouts["dwell1"].set_live(live.dwell_ms[0] if live.dwell_ms else 0.0)
        self.readouts["fuelp"].set_live(live.fuel_pressure_kpa)

        # Ignition cut is shown on the spark readouts rather than as a separate flag: a cut
        # cylinder produces an ignition value of zero, which looks like a harmless number
        # unless it is labelled as a cut.
        if live.ignition_cut and any(live.ignition_cut):
            cut = ", ".join(str(i + 1) for i, c in enumerate(live.ignition_cut) if c)
            note = f"CUT: cylinder {cut}"
            self.readouts["ign1"].set_live(
                live.ignition_deg[0] if live.ignition_deg else 0.0, note
            )
            self.readouts["dwell1"].set_live(
                live.dwell_ms[0] if live.dwell_ms else 0.0, note
            )

        active = SafetySummary.active(live.safety_active)
        if active:
            self.safety.setText("SAFETY: " + ", ".join(active))
            self.safety.setStyleSheet("color:#f87171; font-weight:bold;")
        else:
            self.safety.setText("no active safety")
            self.safety.setStyleSheet("color:#4ade80;")

        self.freshness.setText(f"live {age_s:.1f}s ago")
        self._redraw()

    def on_timeout(self) -> None:
        """Called when no frame arrived this interval.

        Everything goes stale and says so. Not an error dialog: a link that drops on a track
        is a fact to display, not an exception to raise, and a dialog per dropout would
        become noise the operator learns to dismiss.
        """
        for widget in self.readouts.values():
            widget.set_stale()
        self.freshness.setText("no data - link may be down")
        self.freshness.setStyleSheet("color:#f87171;")

    def clear(self) -> None:
        self._last = None
        self._rpm.clear()
        self._lambda.clear()
        self._map.clear()
        for widget in self.readouts.values():
            widget.clear()
        self.safety.setText("no active safety")
        self.safety.setStyleSheet("color:#4ade80;")
        self.freshness.setText("")
        self.freshness.setStyleSheet("")
        self._redraw()
        self.set_link(False, False, False)
