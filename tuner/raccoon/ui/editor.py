"""The calibration editor: 519 items, grouped, searchable, with a table for the big ones.

Two decisions make 519 items navigable rather than merely present:

**A filter that can express a question.** "Show me everything in the fuel group" and "show
me everything that is unverified" and "show me what I have changed" are all the same search
box, because an operator's actual question is rarely "where did the generator put this".

**A change list, always visible.** A tuner that shows a progress bar for a tune change is
offering no way to find out what is about to change. The diff is shown before the write, and
the write is refused until the operator has seen it.

The 2-D tables are the reason a specialist tool exists at all, so they get a real grid with
axis labels rather than a text field per cell.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from raccoon import calibration as cal

CHANGED = QColor("#fbbf24")
UNVERIFIED = QColor("#f87171")
AXIS = QColor("#94a3b8")


class ScalarEditor(QWidget):
    """One scalar: name, value, unit, range, and where its default came from.

    The range and the provenance are shown next to the field, not in a manual. A tuner that
    lets you type 300 into a field whose range is 0..200 and only mentions it on refusal is
    a worse tuner than one that shows the bound before you type.
    """

    changed = pyqtSignal()

    def __init__(self, item: cal.CalibrationItem, image: cal.Calibration) -> None:
        super().__init__()
        self.item = item
        self.image = image
        self._loading = False

        form = QFormLayout(self)
        form.setContentsMargins(8, 6, 8, 6)
        form.setSpacing(4)

        self.field = QDoubleSpinBox()
        self.field.setDecimals(self._decimals())
        self.field.setRange(item.minimum, item.maximum)
        self.field.setSingleStep(self._step())
        self.field.setFont(QFont("DejaVu Sans Mono", 12))
        self.field.setValue(image.get(item))
        self.field.valueChanged.connect(self._on_change)

        unit = QLabel(item.unit or "")
        unit.setStyleSheet("color:#94a3b8;")

        self.field.setToolTip(
            f"{item.label()}\\n"
            f"range {item.minimum:g} .. {item.maximum:g} {item.unit}\\n"
            f"default {item.default:g} ({item.default_source or 'unspecified'})"
        )

        form.addRow(QLabel(item.label()), self.field)
        form.addRow("", unit)

        if item.default_source and item.default_source.lower() != "verified":
            # The colour is a warning, and the reason is on the tooltip. An unverified default
            # is not a broken item - most of a real tune starts that way - but nobody should
            # ship one believing it came from a measurement.
            self.field.setStyleSheet(f"border: 1px solid {UNVERIFIED.name()};")
            unit.setText(f"{item.unit}   (default: {item.default_source})")
            unit.setStyleSheet("color:#f87171;")

    def _decimals(self) -> int:
        # Enough places to show a step the operator can actually set. An integer item shown
        # as "4.0" invites a fumble that a whole-number field would have prevented.
        if self.item.scale >= 1.0 and float(self.item.default).is_integer():
            return 0
        return 2

    def _step(self) -> float:
        step = (self.item.maximum - self.item.minimum) / 1000.0
        if self.item.scale < 1.0:
            step = min(step, self.item.scale)
        return max(step, 10 ** -self._decimals())

    def _on_change(self, value: float) -> None:
        if self._loading:
            return
        try:
            self.image.set(self.item, float(value))
        except ValueError as exc:
            # The spin box's own range should have prevented this, so reaching it means the
            # two disagree - which is worth showing rather than swallowing.
            self.field.setStyleSheet("border: 1px solid #f87171;")
            self.field.setToolTip(str(exc))
            return
        self.changed.emit()

    def refresh(self) -> None:
        self._loading = True
        self.field.setValue(self.image.get(self.item))
        self._loading = False


class TableEditor(QWidget):
    """A 2-D table, with the row and column axes named and a fill button.

    The fill button is there because a real tune is built by filling a table and then shaping
    it, and a 16x16 grid is 256 individual edits otherwise. It is a single undoable step.
    """

    changed = pyqtSignal()

    def __init__(self, item: cal.TableItem, image: cal.Calibration) -> None:
        super().__init__()
        self.item = item
        self.image = image
        self._loading = False

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        header = QHBoxLayout()
        caption = QLabel(f"{item.label()}   [{item.minimum:g} .. {item.maximum:g} {item.unit}]")
        caption.setStyleSheet("color:#94a3b8;")
        header.addWidget(caption)
        header.addStretch(1)

        self.fill = QDoubleSpinBox()
        self.fill.setRange(item.minimum, item.maximum)
        self.fill.setDecimals(2)
        self.fill.setValue(item.default)
        self.fill.setMaximumWidth(90)
        header.addWidget(QLabel("fill"))
        header.addWidget(self.fill)
        fill_button = QPushButton("fill all")
        fill_button.clicked.connect(self._fill)
        header.addWidget(fill_button)
        layout.addLayout(header)

        rows, cols = item.alloc_rows, item.alloc_cols
        self.grid = QTableWidget(rows, cols)
        self.grid.setHorizontalHeaderLabels([str(i) for i in range(cols)])
        self.grid.setVerticalHeaderLabels([str(i) for i in range(rows)])
        self.grid.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.grid.verticalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.grid.setEditTriggers(QAbstractItemView.EditTrigger.AllEditTriggers)
        self.grid.setFont(QFont("DejaVu Sans Mono", 10))
        self.grid.itemChanged.connect(self._on_change)

        axis = QLabel(f"columns: {item.col_axis}    rows: {item.row_axis}    "
                      f"scale x{item.scale:g}")
        axis.setStyleSheet("color:#94a3b8; font-size:11px;")
        layout.addWidget(self.grid)
        layout.addWidget(axis)

        self._load()

    def _load(self) -> None:
        self._loading = True
        cells = self.image.table(self.item)
        for row in range(self.grid.rowCount()):
            for col in range(self.grid.columnCount()):
                index = row * self.grid.columnCount() + col
                text = QTableWidgetItem(f"{cells[index]:g}")
                text.setTextAlignment(Qt.AlignmentFlag.AlignRight)
                if index < len(cells) and not self.item.in_range(cells[index]):
                    # Out of the item's own bounds: impossible through the editor, but
                    # possible from a file, and shown rather than silently corrected.
                    text.setBackground(QColor("#450a0a"))
                    text.setForeground(QColor("#fca5a5"))
                self.grid.setItem(row, col, text)
        self._loading = False

    def _on_change(self, changed: QTableWidgetItem) -> None:
        if self._loading or changed is None:
            return
        try:
            value = float(changed.text())
        except ValueError:
            return
        row, col = changed.row(), changed.column()
        try:
            self.image.set_cell(self.item, row * self.grid.columnCount() + col, value)
        except (ValueError, IndexError) as exc:
            changed.setBackground(QColor("#450a0a"))
            changed.setToolTip(str(exc))
            return
        changed.setBackground(QColor())
        self.changed.emit()

    def _fill(self) -> None:
        try:
            self.image.fill_table(self.item, self.fill.value())
        except ValueError as exc:
            self.fill.setStyleSheet("border: 1px solid #f87171;")
            self.fill.setToolTip(str(exc))
            return
        self._load()
        self.changed.emit()

    def refresh(self) -> None:
        self._load()


class CalibrationEditor(QWidget):
    """The whole editor: a filter, a tree, the selected item, and a diff."""

    changed = pyqtSignal()

    def __init__(self) -> None:
        super().__init__()
        self.image: cal.Calibration | None = None
        self.baseline: cal.Calibration | None = None
        self._editors: list = []

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(8)

        root.addWidget(self._build_filter())

        split = QSplitter(Qt.Orientation.Horizontal)
        self.split = split
        split.addWidget(self._build_tree())
        self.detail = QWidget()
        self.detail_layout = QVBoxLayout(self.detail)
        self.detail_layout.setContentsMargins(0, 0, 0, 0)
        self._show_placeholder("Select an item on the left to edit it. "
                               "Search filters the tree; 'only what I have changed' shows "
                               "the diff to the ECU.")
        split.addWidget(self.detail)
        # Roughly half and half, and the tree keeps at least its minimum. The first version
        # let the detail pane take the stretch, which left a wide empty area beside a tree
        # clipped to a few columns - the exact opposite of what makes 519 items navigable.
        split.setStretchFactor(0, 1)
        split.setStretchFactor(1, 1)
        split.setSizes([560, 560])
        root.addWidget(split, stretch=1)

        root.addWidget(self._build_diff())

    # ------------------------------------------------------------------ filter

    def _build_filter(self) -> QWidget:
        box = QGroupBox("find")
        layout = QHBoxLayout(box)
        layout.setContentsMargins(8, 6, 8, 6)

        self.search = QLineEdit()
        self.search.setPlaceholderText(
            "group, name or unit - e.g. ve, fuel, ignition, ms, unverified"
        )
        self.search.textChanged.connect(self._apply_filter)
        layout.addWidget(self.search, stretch=1)

        self.only_changed = QCheckBox("only what I have changed")
        self.only_changed.stateChanged.connect(self._apply_filter)
        layout.addWidget(self.only_changed)

        self.only_unverified = QCheckBox("only unverified defaults")
        self.only_unverified.stateChanged.connect(self._apply_filter)
        layout.addWidget(self.only_unverified)

        return box

    def _build_tree(self) -> QWidget:
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["item", "value", "unit"])
        # Stretch the name column and let the last one take the remainder. Fixed widths on a
        # tree of 519 items is a nuisance: long names are truncated and the column cannot
        # follow the splitter.
        header = self.tree.header()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.setTextElideMode(Qt.TextElideMode.ElideMiddle)
        self.tree.setUniformRowHeights(True)
        self.tree.setMinimumWidth(340)
        self.tree.currentItemChanged.connect(self._on_selected)
        self.tree.itemDoubleClicked.connect(lambda *_: self._on_selected(*self._current()))
        return self.tree

    def _build_diff(self) -> QWidget:
        box = QGroupBox("this tune differs from the ECU's, in these items")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 6, 8, 6)
        self.diff = QLabel("nothing changed yet")
        self.diff.setWordWrap(True)
        self.diff.setStyleSheet("color:#94a3b8;")
        layout.addWidget(self.diff)
        return box

    # ------------------------------------------------------------------ loading

    def load(self, image: cal.Calibration, baseline: cal.Calibration | None = None) -> None:
        self.image = image
        # A SEPARATE object, always.
        #
        # Passing the same calibration as both the working copy and the baseline makes every
        # comparison trivially equal: the diff is permanently empty, the write button never
        # enables, and the whole edit-then-write path is dead while looking perfectly healthy
        # - the tree shows your new value, the panel says "matches the ECU's", and nothing
        # explains why. The baseline is a snapshot of what the ECU is serving, so it has to
        # be one.
        self.baseline = baseline or cal.Calibration.from_bytes(image.to_bytes())
        self._rebuild_tree()
        self._refresh_diff()
        # Re-filter against the new image, so a stale "only what I have changed" view does
        # not survive a re-read and claim items are still different.
        self._apply_filter()

    def _rebuild_tree(self) -> None:
        self.tree.clear()
        if self.image is None:
            return
        groups: dict[str, QTreeWidgetItem] = {}
        for group in cal.GROUPS:
            node = QTreeWidgetItem([group, "", ""])
            self.tree.addTopLevelItem(node)
            groups[group] = node
        for item in cal.ITEMS:
            node = groups.get(item.group)
            if node is None:
                continue
            child = QTreeWidgetItem([item.name, self._value_text(item), item.unit])
            child.setData(0, Qt.ItemDataRole.UserRole, item.id)
            if item.default_source and item.default_source.lower() != "verified":
                child.setForeground(1, UNVERIFIED)
            node.addChild(child)
        self.tree.expandToDepth(0)
        self._apply_filter()

    def _value_text(self, item: cal.CalibrationItem) -> str:
        assert self.image is not None
        if item.is_table:
            return f"{item.alloc_rows}x{item.alloc_cols}"
        if item.is_axis:
            points = self.image.axis(item)
            span = f"{points[0]:g}-{points[-1]:g}" if points else "?"
            return f"{len(points)} pts {span}"
        return f"{self.image.get(item):g}"

    def _apply_filter(self) -> None:
        """Filter on the text, the changed flag, and the provenance.

        A tree that hides the 500 items an operator is not currently working on is what makes
        519 items feel like 519 items. Hiding them by default and showing matches on demand is
        what makes it feel like a handful.
        """
        needle = self.search.text().strip().lower()
        terms = needle.split()
        want_changed = self.only_changed.isChecked()
        want_unverified = self.only_unverified.isChecked()
        assert self.image is not None and self.baseline is not None

        for group in cal.GROUPS:
            node = None
            for i in range(self.tree.topLevelItemCount()):
                candidate = self.tree.topLevelItem(i)
                if candidate.text(0) == group:
                    node = candidate
                    break
            if node is None:
                continue
            any_visible = False
            for j in range(node.childCount()):
                child = node.child(j)
                haystack = f"{group}.{child.text(0)} {child.text(2)}".lower()
                matches = all(term in haystack for term in terms)
                if want_changed:
                    item = cal.ITEMS_BY_ID.get(child.data(0, Qt.ItemDataRole.UserRole))
                    matches = matches and item is not None and self._is_changed(item)
                if want_unverified:
                    item = cal.ITEMS_BY_ID.get(child.data(0, Qt.ItemDataRole.UserRole))
                    matches = matches and bool(item and item.default_source
                                               and item.default_source.lower() != "verified")
                child.setHidden(not matches)
                any_visible = any_visible or matches
            node.setHidden(not any_visible)
            if any_visible and terms:
                node.setExpanded(True)

    def _is_changed(self, item: cal.CalibrationItem) -> bool:
        assert self.image is not None and self.baseline is not None
        if item.is_table:
            return self.image.table(item) != self.baseline.table(item)
        return abs(self.image.get(item) - self.baseline.get(item)) > 1e-9

    # ---------------------------------------------------------------- selection

    def _current(self):
        item = self.tree.currentItem()
        return (self.tree, item) if item is not None else (self.tree, None)

    def _clear_detail(self) -> None:
        while self.detail_layout.count():
            child = self.detail_layout.takeAt(0).widget()
            if child is not None:
                child.deleteLater()

    def _show_placeholder(self, text: str) -> None:
        self._clear_detail()
        label = QLabel(text)
        label.setWordWrap(True)
        label.setStyleSheet("color:#64748b;")
        self.detail_layout.addWidget(label)
        self.detail_layout.addStretch(1)

    def _on_selected(self, *_args) -> None:
        node = self.tree.currentItem()
        if node is None or node.parent() is None or self.image is None:
            return
        item = cal.ITEMS_BY_ID.get(node.data(0, Qt.ItemDataRole.UserRole))
        if item is None:
            return
        self._clear_detail()
        self._editors = []
        if item.is_table:
            editor = TableEditor(item, self.image)  # type: ignore[arg-type]
        else:
            editor = ScalarEditor(item, self.image)
        editor.changed.connect(self._on_edited)
        self._editors.append(editor)
        self.detail_layout.addWidget(editor)

    def _on_edited(self) -> None:
        self._rebuild_values()
        self._refresh_diff()
        self.changed.emit()

    def _rebuild_values(self) -> None:
        """Update the value column in place after an edit, without rebuilding the tree.

        Rebuilding would collapse the selection and lose the scroll position, which is
        exactly what an operator does not want after typing a value.
        """
        if self.image is None:
            return
        for i in range(self.tree.topLevelItemCount()):
            node = self.tree.topLevelItem(i)
            for j in range(node.childCount()):
                child = node.child(j)
                item = cal.ITEMS_BY_ID.get(child.data(0, Qt.ItemDataRole.UserRole))
                if item is not None and not item.is_table:
                    child.setText(1, f"{self.image.get(item):g}")

    def _refresh_diff(self) -> None:
        """Say exactly what a write would change.

        This is the point of the panel. A tune change with no preview is a way to lose a
        tune that worked, and the operator has no way to notice until the engine does.
        """
        if self.image is None or self.baseline is None:
            self.diff.setText("nothing loaded")
            return
        changed = self.image.differs_from(self.baseline)
        if not changed:
            self.diff.setText("nothing changed yet")
            self.diff.setStyleSheet("color:#94a3b8;")
            return
        described = []
        for item in changed[:12]:
            if item.is_table:
                cells = sum(
                    1 for a, b in zip(self.image.table(item), self.baseline.table(item))  # type: ignore[arg-type]
                    if abs(a - b) > 1e-9
                )
                described.append(f"{item.group}.{item.name} ({cells} cells)")
            else:
                described.append(
                    f"{item.group}.{item.name} "
                    f"{self.baseline.get(item):g} -> {self.image.get(item):g}"
                )
        more = f"  (+{len(changed) - 12} more)" if len(changed) > 12 else ""
        self.diff.setText("  |  ".join(described) + more)
        self.diff.setStyleSheet("color:#fbbf24;")

    def changed_items(self) -> list:
        if self.image is None or self.baseline is None:
            return []
        return self.image.differs_from(self.baseline)
