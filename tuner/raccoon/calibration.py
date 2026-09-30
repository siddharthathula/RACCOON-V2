"""RACCOON V2 - the calibration model the tuner edits and writes.

Built from configs/calibration_descriptor.json, which is itself generated from
configs/schema/*.yaml - the same source that generates the firmware's cal_types.h,
cal_access.h and cal_descriptors.c. Nothing here is a second opinion about an item's id,
offset, scale or range; it is the same data, loaded.

A hand-written second copy of the calibration is how a tuner ends up writing a value the
firmware reads as a different one. The write succeeds, the tune is accepted, and the engine
runs something nobody intended. Deriving both sides from one schema is the only way to make
that impossible rather than merely unlikely.
"""

from __future__ import annotations

import json
import pathlib
import struct
from dataclasses import dataclass, field
from typing import Any

_ROOT = pathlib.Path(__file__).resolve().parents[2]
_DESCRIPTOR = _ROOT / "configs" / "calibration_descriptor.json"

# Schema field type -> struct character, and its width in bytes.
# The struct character for a stored value. The WIDTH is not taken from the type name but
# from the descriptor's recorded `size`, because the type name does not always say how wide
# it is: a bitfield of more than eight bits is stored in a uint16_t, and reading it as one
# byte shifts every field after it by one and produces an image that is still the right
# size for everything before the mistake.
_CHAR_FOR_TYPE = {
    "f32": "f", "u8": "B", "u16": "H", "u32": "I",
    "i8": "b", "i16": "h", "i32": "i",
    "enum": "B", "bits": "B", "bool": "B",
}
_CHAR_FOR_WIDTH = {1: "B", 2: "H", 4: "I"}


def _width_of(item_type: str, width_bytes: int) -> tuple[str, int]:
    """(struct character, width in bytes) for a stored value.

    The width comes from the descriptor's `bytes` field, which the generator emits from the
    C type. It cannot be derived from the schema type name alone: a bitfield of more than
    eight bits is stored in a uint16_t while a narrower one is a uint8_t, and a bitfield
    that is read as the wrong width shifts every field after it.
    """
    if item_type == "f32":
        return "f", 4
    if item_type == "bool":
        return "B", 1
    if item_type not in _CHAR_FOR_TYPE:
        raise KeyError(f"no struct mapping for calibration type {item_type}")
    char = _CHAR_FOR_WIDTH.get(width_bytes)
    if char is None:
        raise ValueError(f"unsupported stored width {width_bytes} bytes for {item_type}")
    return char, width_bytes


@dataclass
class CalibrationItem:
    """One calibration item, exactly as the firmware describes it."""

    name: str
    group: str
    id: int
    offset: int
    size: int
    bytes: int
    type: str
    unit: str
    scale: float
    minimum: float
    maximum: float
    default: float
    owner: str
    safety_class: str
    default_source: str
    description: str
    live_edit: bool
    # Table axes, for the 2D editors.
    alloc_rows: int = 0
    alloc_cols: int = 0
    cell_type: str = ""
    row_axis: str = ""
    col_axis: str = ""
    # The axis1d ITEMS holding this table's breakpoints, as ids. row_axis/col_axis above
    # name the axis KIND ("rpm") and resolve to no item; these two resolve.
    row_axis_id: int | None = None
    col_axis_id: int | None = None
    row_max: float = 0.0
    col_max: float = 0.0
    alloc_count: int = 0
    axis_first: float = 0.0
    enum_values: list[tuple[str, int]] = field(default_factory=list)
    dependencies: list[str] = field(default_factory=list)

    @property
    def is_table(self) -> bool:
        return self.alloc_rows > 0 and self.alloc_cols > 0

    @property
    def is_axis(self) -> bool:
        return self.alloc_count > 0

    @property
    def cell_count(self) -> int:
        if self.alloc_count > 0:
            return self.alloc_count
        return self.alloc_rows * self.alloc_cols

    def default_points(self) -> list[float]:
        """An axis's shipped breakpoints: even across the declared range, rounded.

        Reproduces rc_cal_apply_axis_defaults() exactly. The Python default image is
        compared byte for byte against the firmware's, so a rounding difference here is
        a CRC mismatch on every calibration the tuner has ever written.
        """
        n = self.alloc_count
        if n <= 0:
            return []
        if n == 2:
            return [float(self.axis_first), float(self.maximum)]
        span = float(self.maximum) - float(self.minimum)
        last = n - 1
        # Rounded, not truncated, to match Item.default_points() in tools/codegen/rc_schema.py.
        return [round(float(self.minimum) + span * i / last) for i in range(n)]

    def label(self) -> str:
        unit = f" [{self.unit}]" if self.unit else ""
        return f"{self.group}.{self.name}{unit}"

    def to_raw(self, engineering: float) -> float:
        """Engineering value -> stored value."""
        return engineering / self.scale if self.scale else engineering

    def to_eng(self, raw: float) -> float:
        """Stored value -> engineering value."""
        return raw * self.scale if self.scale else raw

    def in_range(self, engineering: float) -> bool:
        """Range check, refusing NaN.

        NaN fails every comparison, so a naive `min <= v <= max` is False for NaN and it
        slips through as "out of range" - which is right - but a naive `if v < min or
        v > max` is False too, and that admits it. Written as a positive test for
        validity so there is one way to be wrong, not two.
        """
        v = float(engineering)
        if v != v:
            return False
        return self.minimum <= v <= self.maximum


@dataclass
class TableItem(CalibrationItem):
    pass


@dataclass
class AxisItem(CalibrationItem):
    """A 1-D array of breakpoints - the coordinates one of this table's axes runs along.

    An axis is not a scalar and not a table, which is why it is its own type here and in
    the firmware: a host that treats it as either one will index the wrong number of
    things, and the resulting image has the right length and the wrong values in it.
    """


def _descriptor() -> dict:
    """The generated descriptor, read once.

    Generated rather than hand-maintained, so the tuner's idea of the image can only ever
    be the firmware's. Reading it at import is deliberate: a tuner that starts against a
    stale descriptor and discovers the mismatch halfway through a flash is worse than one
    that refuses to start.
    """
    return json.loads(_DESCRIPTOR.read_text())


# The cross-item rules, in the firmware's own order. Only their NAMES and TEXT are needed
# here - the checks themselves run on the target, and reimplementing them in Python is
# exactly the kind of second opinion that disagrees with the first one eventually.
RULES: list[dict] = _descriptor().get("rules", [])


def _axis_id(value: str | None) -> int | None:
    """The descriptor writes axis ids as hex strings, or null when a table has no axis."""
    return None if value is None else int(value, 16)


def _load() -> tuple[int, list[CalibrationItem]]:
    """Build the item list from the generated descriptor.

    Grouped by the first path segment of the item's own name, which is the same grouping
    the firmware and the schema use, rather than a grouping invented here.
    """
    raw = _descriptor()
    items: list[CalibrationItem] = []
    for entry in raw["items"]:
        if entry.get("is_axis"):
            items.append(AxisItem(
                name=entry["name"], group=entry["group"], id=int(entry["id"], 16),
                offset=entry["offset"], size=entry["size"], bytes=int(entry.get("bytes", 1)),
                type=entry["type"],
                unit=entry.get("unit", ""), scale=float(entry["scale"]),
                minimum=float(entry["min"]), maximum=float(entry["max"]),
                default=float(entry["default"]),
                owner=entry["owner"], safety_class=entry["safety_class"],
                default_source=entry.get("default_source", ""),
                description=entry.get("description", ""),
                live_edit=bool(entry.get("live_edit", False)),
                cell_type=entry.get("cell_type", "u16"),
                row_axis=entry.get("axis", ""),
                alloc_count=int(entry["alloc_count"]),
                axis_first=float(entry["default"]),
                dependencies=entry.get("dependencies", []),
            ))
        elif entry.get("alloc_rows"):
            items.append(TableItem(
                name=entry["name"], group=entry["group"], id=int(entry["id"], 16),
                offset=entry["offset"], size=entry["size"], bytes=int(entry.get("bytes", 1)),
                type=entry["type"],
                unit=entry.get("unit", ""), scale=float(entry["scale"]),
                minimum=float(entry["min"]), maximum=float(entry["max"]),
                default=float(entry["default"]),
                owner=entry["owner"], safety_class=entry["safety_class"],
                default_source=entry.get("default_source", ""),
                description=entry.get("description", ""),
                live_edit=bool(entry.get("live_edit", False)),
                alloc_rows=entry["alloc_rows"], alloc_cols=entry["alloc_cols"],
                cell_type=entry.get("cell_type", "u8"),
                row_axis=entry.get("row_axis", ""), col_axis=entry.get("col_axis", ""),
                row_axis_id=_axis_id(entry.get("row_axis_id")),
                col_axis_id=_axis_id(entry.get("col_axis_id")),
                dependencies=entry.get("dependencies", []),
            ))
        else:
            items.append(CalibrationItem(
                name=entry["name"], group=entry["group"], id=int(entry["id"], 16),
                offset=entry["offset"], size=entry["size"], bytes=int(entry.get("bytes", 1)),
                type=entry["type"],
                unit=entry.get("unit", ""), scale=float(entry["scale"]),
                minimum=float(entry["min"]), maximum=float(entry["max"]),
                default=float(entry["default"]),
                owner=entry["owner"], safety_class=entry["safety_class"],
                default_source=entry.get("default_source", ""),
                description=entry.get("description", ""),
                live_edit=bool(entry.get("live_edit", False)),
                enum_values=[(v["name"], int(v["value"]))
                             for v in entry.get("values", [])],
                dependencies=entry.get("dependencies", []),
            ))
    return int(raw["image_size"]), items


IMAGE_SIZE, ITEMS = _load()
ITEMS_BY_ID: dict[int, CalibrationItem] = {i.id: i for i in ITEMS}
# Full dotted names, built from the id the firmware uses, so "engine.cylinder_count"
# resolves the same way it does in the schema.
ITEMS_BY_NAME: dict[str, CalibrationItem] = {
    f"{i.group}_{i.name}": i for i in ITEMS
}
ITEMS_BY_NAME.update({f"{i.group}.{i.name}": i for i in ITEMS})
TABLES = [i for i in ITEMS if i.is_table]
AXES = [i for i in ITEMS if i.is_axis]
SCALARS = [i for i in ITEMS if not i.is_table and not i.is_axis]

GROUPS: dict[str, list[CalibrationItem]] = {}
for _item in ITEMS:
    GROUPS.setdefault(_item.group, []).append(_item)


# ------------------------------------------------------------------ image layout

# The image is a C struct laid out with natural alignment, so it CONTAINS PADDING between
# items and the sum of the field sizes is smaller than the image. Every item therefore
# carries its own byte offset and is packed there individually.
#
# Building one giant struct format instead looks simpler and is wrong: a format string has no
# way to express "skip these three bytes", so every field after the first padding hole would
# be written three bytes early. The image would still be exactly the right size - the
# padding would just be zeros somewhere else - and the ECU would reject it with a CRC error
# that points at nothing in particular.
#
# This is why the offsets come from the descriptor rather than being computed here: they
# come from the same _Static_assert(offsetof(...)) the C generator emits, so a layout change
# is impossible to get wrong on one side only.

_PACKERS: dict[tuple[str, int], struct.Struct] = {}


def _packer(cell_type: str, width: int) -> struct.Struct:
    key = (cell_type, width)
    if key not in _PACKERS:
        char, actual = _width_of(cell_type, width)
        _PACKERS[key] = struct.Struct("<" + char)
    return _PACKERS[key]


def _total_bytes() -> int:
    """Highest offset plus size, so the image is exactly as long as the last item needs."""
    end = 0
    for item in ITEMS:
        if item.is_table or item.is_axis:
            end = max(end, item.offset + item.cell_count * item.bytes)
        else:
            end = max(end, item.offset + item.bytes)
    return end


def _check_layout() -> None:
    used = _total_bytes()
    if used > IMAGE_SIZE:
        # Loud, immediate, and fatal. A layout that does not fit still packs and still
        # unpacks - it just writes past the end, or addresses the wrong bytes for everything
        # after the mismatch, and the result is an image the ECU rejects for no visible
        # reason.
        raise SystemExit(
            f"calibration layout mismatch: the items occupy {used} bytes but the firmware "
            f"image is only {IMAGE_SIZE}. The offsets in the descriptor and the image size "
            f"disagree - regenerate both from the schema."
        )
    # A C struct's size is rounded up to its own alignment, so the image is a few bytes
    # longer than the last item needs. Those tail bytes are always zero, and the image
    # length always has to be the C size, not the used length: a short image has a
    # different CRC from a padded one holding the same values.
    tail = IMAGE_SIZE - used
    if tail > 3:
        raise SystemExit(
            f"calibration layout looks wrong: the image is {IMAGE_SIZE} bytes but the items "
            f"fill {used} of them. More than 3 bytes of tail cannot be struct alignment, so "
            f"the descriptor and the image size disagree - regenerate both from the schema."
        )


_check_layout()


def rule_text(index: int) -> str:
    """Prose for a cross-item rule, for the tuner's refusal dialog.

    The rules themselves live in the firmware and are enforced there. This only turns the
    index the ECU sent back into something readable, so the two never have to agree about
    what a rule MEANS - only about which one fired, which the index settles.
    """
    for rule in RULES:
        if rule["index"] == index:
            return rule.get("text") or rule.get("name", "")
    return f"rule {index}"


def default_image() -> bytes:
    """The bytes the ECU ships with, reconstructed from the same defaults the C uses."""
    blob = bytearray(IMAGE_SIZE)
    for item in ITEMS:
        if item.is_table:
            _pack_table(blob, item, [item.default] * item.cell_count)
        elif item.is_axis:
            _pack_table(blob, item, item.default_points())
        else:
            _pack_scalar(blob, item, item.default)
    return bytes(blob)


def _pack_scalar(blob: bytearray, item: CalibrationItem, engineering: float) -> None:
    char, width = _width_of(item.type, item.bytes)
    raw = item.to_raw(engineering)
    if char == "f":
        struct.pack_into("<f", blob, item.offset, raw)
        return
    # Integers are stored raw, and clamped to their own width so a value the schema allows
    # cannot wrap on the way in and become a different number entirely.
    value = int(round(engineering)) if item.scale == 1.0 else int(round(raw))
    signed = char in ("b", "h", "i")
    bits = width * 8
    lo = -(1 << (bits - 1)) if signed else 0
    hi = ((1 << (bits - 1)) - 1) if signed else ((1 << bits) - 1)
    struct.pack_into("<" + char, blob, item.offset, max(lo, min(hi, value)))


def _unpack_scalar(blob: bytes, item: CalibrationItem) -> float:
    char, _ = _width_of(item.type, item.bytes)
    raw = struct.unpack_from("<" + char, blob, item.offset)[0]
    if char == "f":
        return item.to_eng(float(raw))
    return float(raw)


def _pack_table(blob: bytearray, item: TableItem, cells: list[float]) -> None:
    char, width = _width_of(item.cell_type, item.bytes)
    signed = char in ("b", "h", "i")
    bits = width * 8
    lo = -(1 << (bits - 1)) if signed else 0
    hi = ((1 << (bits - 1)) - 1) if signed else ((1 << bits) - 1)
    for i, value in enumerate(cells[: item.cell_count]):
        raw = item.to_raw(float(value))
        if char == "f":
            struct.pack_into("<f", blob, item.offset + i * width, raw)
        else:
            struct.pack_into("<" + char, blob, item.offset + i * width,
                             max(lo, min(hi, int(round(raw)))))


def _unpack_table(blob: bytes, item: TableItem) -> list[float]:
    char, width = _width_of(item.cell_type, item.bytes)
    out: list[float] = []
    for i in range(item.cell_count):
        raw = struct.unpack_from("<" + char, blob, item.offset + i * width)[0]
        out.append(item.to_eng(float(raw)) if char == "f" else float(raw))
    return out


def _pack_axis(blob: bytearray, item: CalibrationItem, points: list[float]) -> None:
    """Write an axis' breakpoints. The width comes from `cell_type`, not from the item's
    own type name, which is the string "axis1d" and describes no storage at all."""
    char, width = _width_of(item.cell_type, item.bytes)
    for i, value in enumerate(points[: item.alloc_count]):
        raw = item.to_raw(float(value))
        if char == "f":
            struct.pack_into("<f", blob, item.offset + i * width, raw)
        else:
            struct.pack_into("<" + char, blob, item.offset + i * width, int(round(raw)))


def _unpack_axis(blob: bytes, item: CalibrationItem) -> list[float]:
    char, width = _width_of(item.cell_type, item.bytes)
    out: list[float] = []
    for i in range(item.alloc_count):
        raw = struct.unpack_from("<" + char, blob, item.offset + i * width)[0]
        out.append(item.to_eng(float(raw)) if char == "f" else float(raw))
    return out


class Calibration:
    """An in-memory calibration image, with the same range rules the ECU applies.

    A write through this class is refused when the value is out of the item's own range, so
    an out-of-range value cannot reach the flash at all. Cross-item RULES are the ECU's job
    and are not duplicated here: they are enforced on the target, at stage time, and the
    result is reported back. A second implementation of those rules here would inevitably
    disagree with the firmware's, and the disagreement would be invisible.
    """

    def __init__(self, blob: bytes | None = None) -> None:
        self._blob = bytearray(blob if blob is not None else default_image())
        if len(self._blob) < IMAGE_SIZE:
            self._blob.extend(bytes(IMAGE_SIZE - len(self._blob)))
        self._tables: dict[int, list[float]] = {
            t.id: _unpack_table(bytes(self._blob), t) for t in TABLES
        }
        self._axes: dict[int, list[float]] = {
            a.id: _unpack_axis(bytes(self._blob), a) for a in AXES
        }

    @classmethod
    def from_bytes(cls, blob: bytes) -> "Calibration":
        if len(blob) < IMAGE_SIZE:
            raise ValueError(
                f"calibration image is {len(blob)} bytes, the ECU expects {IMAGE_SIZE}"
            )
        return cls(blob[:IMAGE_SIZE])

    def to_bytes(self) -> bytes:
        for item in TABLES:
            _pack_table(self._blob, item, self._tables[item.id])
        for item in AXES:
            _pack_axis(self._blob, item, self._axes[item.id])
        return bytes(self._blob)

    def get(self, item: CalibrationItem | str) -> float:
        it = self._resolve(item)
        if it.is_table:
            raise TypeError(f"{it.label()} is a table; use table()")
        if it.is_axis:
            raise TypeError(f"{it.label()} is an axis; use axis()")
        return _unpack_scalar(bytes(self._blob), it)

    def set(self, item: CalibrationItem | str, value: float) -> None:
        """Write one scalar. Refuses a value outside the item's own range."""
        it = self._resolve(item)
        if it.is_table:
            raise TypeError(f"{it.label()} is a table; use set_cell()")
        if it.is_axis:
            raise TypeError(f"{it.label()} is an axis; use set_point()")
        if not it.in_range(value):
            raise ValueError(
                f"{it.label()} = {value} is outside [{it.minimum}, {it.maximum}]"
            )
        _pack_scalar(self._blob, it, float(value))

    def table(self, item: CalibrationItem | str) -> list[float]:
        it = self._resolve(item)
        if not it.is_table:
            raise TypeError(f"{it.label()} is not a table")
        return list(self._tables[it.id])

    def row_axis_of(self, table: CalibrationItem | str) -> list[float]:
        """The breakpoints along a table's row dimension."""
        it = self._resolve(table)
        if not it.is_table or it.row_axis_id is None:
            raise TypeError(f"{it.label()} has no row axis")
        return self.axis(it.row_axis_id)

    def col_axis_of(self, table: CalibrationItem | str) -> list[float]:
        """The breakpoints along a table's column dimension."""
        it = self._resolve(table)
        if not it.is_table or it.col_axis_id is None:
            raise TypeError(f"{it.label()} has no column axis")
        return self.axis(it.col_axis_id)

    def axis(self, item: CalibrationItem | str | int) -> list[float]:
        """An axis' breakpoints, low to high, by name or by id."""
        it = self._resolve(item)
        if not it.is_axis:
            raise TypeError(f"{it.label()} is not an axis")
        return list(self._axes[it.id])

    def set_point(self, item: CalibrationItem | str, index: int, value: float) -> None:
        """Write one breakpoint.

        Range-checked against the axis' own declared range, but NOT against its
        neighbours: ordering is a property of the whole axis and is checked when the
        image is validated. Refusing an edit that crosses a neighbour here would make it
        impossible to drag a breakpoint past the one next to it, which is the single most
        common axis edit there is.
        """
        it = self._resolve(item)
        if not it.is_axis:
            raise TypeError(f"{it.label()} is not an axis")
        if not 0 <= index < it.alloc_count:
            raise IndexError(
                f"breakpoint {index} is outside the {it.alloc_count} of {it.label()}"
            )
        if not it.in_range(value):
            raise ValueError(
                f"{it.label()}[{index}] = {value} is outside "
                f"[{it.minimum}, {it.maximum}]"
            )
        self._axes[it.id][index] = float(value)

    def axis_is_monotonic(self, item: CalibrationItem | str) -> bool:
        """True when every breakpoint is strictly greater than the one before it.

        An axis with a repeated or inverted pair cannot be searched with a binary or
        bilinear lookup: it either loops or returns whichever cell it happened to land in.
        """
        it = self._resolve(item)
        if not it.is_axis:
            raise TypeError(f"{it.label()} is not an axis")
        points = self._axes[it.id]
        if len(points) < 2:
            return False
        return all(b > a for a, b in zip(points, points[1:]))

    def set_cell(self, item: CalibrationItem | str, index: int, value: float) -> None:
        it = self._resolve(item)
        if not it.is_table:
            raise TypeError(f"{it.label()} is not a table")
        if not 0 <= index < it.cell_count:
            raise IndexError(f"cell {index} is outside the {it.cell_count} of {it.label()}")
        if not it.in_range(value):
            raise ValueError(
                f"{it.label()}[{index}] = {value} is outside [{it.minimum}, {it.maximum}]"
            )
        self._tables[it.id][index] = float(value)

    def fill_table(self, item: CalibrationItem | str, value: float) -> None:
        it = self._resolve(item)
        if not it.is_table:
            raise TypeError(f"{it.label()} is not a table")
        if not it.in_range(value):
            raise ValueError(
                f"{it.label()} = {value} is outside [{it.minimum}, {it.maximum}]"
            )
        self._tables[it.id] = [float(value)] * it.cell_count

    def differs_from(self, other: "Calibration") -> list[CalibrationItem]:
        """Items that differ. Used to show the operator exactly what a flash would change.

        Showing the diff before writing, rather than a progress bar, is the difference
        between a tuner and a way to lose a tune.
        """
        changed: list[CalibrationItem] = []
        for item in SCALARS:
            if abs(self.get(item) - other.get(item)) > 1e-6:
                changed.append(item)
        for table in TABLES:
            if self.table(table) != other.table(table):
                changed.append(table)
        for axis in AXES:
            if self.axis(axis) != other.axis(axis):
                changed.append(axis)
        return changed

    def _resolve(self, item: CalibrationItem | str | int) -> CalibrationItem:
        if isinstance(item, str):
            found = ITEMS_BY_NAME.get(item)
            if found is None:
                raise KeyError(f"no such calibration item: {item}")
            return found
        if isinstance(item, int):
            found = ITEMS_BY_ID.get(item)
            if found is None:
                raise KeyError(f"no calibration item with id 0x{item:04X}")
            return found
        return item
