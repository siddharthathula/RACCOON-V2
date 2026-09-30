"""RACCOON V2 calibration schema model, parser and validator.

Single source of truth: ``configs/schema/*.yaml``.  Everything else - the firmware C
types and defaults, the tuner Python types, the GUI descriptor and the reference
documentation - is generated from this.

Design rules enforced here (see docs/CALIBRATION_SPEC.md):

* every item carries a **stable explicit 16-bit id**; ids are never positional, so
  adding an item can never move an existing one;
* ``default``/``min``/``max`` in the schema are written in **ENGINEERING units** (what
  a tuner sees).  The generator converts them to the raw storage representation, and
  refuses to emit if the conversion is not exact for an integer-typed item;
* every item carries ``unit``, ``default_source`` and an optional ``source_ref``, so an
  unmeasured value stays visibly unmeasured all the way to the tuner UI;
* tables are allocated at their maximum size and their active shape is taken from
  companion dimension items, so a 4-cylinder build can drive a 16x16 map out of a
  32x32 allocation;
* the byte layout is computed here and re-proved in C by
  ``_Static_assert(offsetof(...) == ...)``, so a layout bug is a compile error rather
  than a silently scrambled calibration.

Depends on PyYAML only.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

SCHEMA_VERSION = 1

#: Provenance vocabulary, mirroring docs/CALIBRATION_SPEC.md section 1.
SOURCES = (
    "SOURCE-DERIVED",
    "CALCULATED",
    "MEASURED",
    "SIMULATED",
    "HIL-VERIFIED",
    "BENCH-MEASURED",
    "ENGINE-TESTED",
    "DYNO-MEASURED",
    "VEHICLE-VALIDATED",
    "UNVERIFIED",
)

OWNERS = ("READ_ONLY", "ENGINEER", "TUNER", "SAFETY", "MODE")
SAFETY_CLASSES = ("INFORMATIONAL", "MEDIUM", "HIGH", "CRITICAL")

#: Engineering-to-raw domain per scalar type.
SCALAR_TYPES = {
    "bool": ("uint8_t", 1, 0.0, 1.0),
    "u8": ("uint8_t", 1, 0.0, 255.0),
    "i8": ("int8_t", 1, -128.0, 127.0),
    "u16": ("uint16_t", 2, 0.0, 65535.0),
    "i16": ("int16_t", 2, -32768.0, 32767.0),
    "u32": ("uint32_t", 4, 0.0, 4294967295.0),
    "i32": ("int32_t", 4, -2147483648.0, 2147483647.0),
    "f32": ("float", 4, -3.4028234663852886e38, 3.4028234663852886e38),
}

INTEGER_TYPES = ("u8", "i8", "u16", "i16", "u32", "i32")

#: Natural alignment of each C scalar type, used to lay the image out exactly as a
#: C compiler would.
C_ALIGNMENT = {
    "uint8_t": 1,
    "int8_t": 1,
    "uint16_t": 2,
    "int16_t": 2,
    "uint32_t": 4,
    "int32_t": 4,
    "float": 4,
}

PROVENANCE_REQUIRING_SOURCE = {
    "MEASURED",
    "HIL-VERIFIED",
    "BENCH-MEASURED",
    "ENGINE-TESTED",
    "DYNO-MEASURED",
    "VEHICLE-VALIDATED",
}


class SchemaError(Exception):
    """Any schema violation. Always fatal for the build."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise SchemaError(message)


@dataclass(frozen=True)
class EnumValue:
    name: str
    value: int
    description: str = ""


@dataclass
class Item:
    """One calibration item: a scalar, a bitfield, an enum or a 2D table."""

    name: str
    id: int
    type: str
    group: str = ""
    unit: str = ""
    description: str = ""

    # Engineering values, exactly as written in the schema.
    default: Any = 0
    minimum: Any = 0
    maximum: Any = 1

    # Raw storage values, derived by the generator.
    raw_default: int | float = 0
    raw_minimum: int | float = 0
    raw_maximum: int | float = 1

    scale: float = 1.0
    owner: str = "TUNER"
    safety_class: str = "INFORMATIONAL"
    default_source: str = "UNVERIFIED"
    source_ref: str = ""
    dependencies: list[str] = field(default_factory=list)
    live_edit: bool = True
    derived: bool = False

    values: list[EnumValue] = field(default_factory=list)
    bits: list[str] = field(default_factory=list)

    alloc_rows: int = 0
    alloc_cols: int = 0
    alloc_count: int = 0        # axis1d: how many breakpoints
    default_fill: float = 0.0  # value every cell is initialised to, in engineering units
    row_axis: str = ""
    col_axis: str = ""
    cell_type: str = "u8"
    cell_min: float = 0.0
    cell_max: float = 255.0

    offset: int = 0

    # ---------------------------------------------------------------- helpers
    @property
    def is_table(self) -> bool:
        return self.type == "table2d"

    def default_points(self) -> list[float]:
        """An axis' shipped breakpoints: even across the declared range, rounded.

        This is the single definition the C generator, the Python model and the build-time
        rule all read. They have to agree exactly, because the Python default image is
        compared byte for byte against the firmware's and a rounding difference is a CRC
        mismatch on every calibration the tuner has ever written.
        """
        n = self.alloc_count
        if n <= 0:
            return []
        if n == 2:
            return [float(self.default_fill), float(self.cell_max)]
        span = self.cell_max - self.cell_min
        last = n - 1
        return [round(self.cell_min + span * i / last) for i in range(n)]

    @property
    def is_axis(self) -> bool:
        """A 1-D array of axis BREAKPOINTS rather than a value table.

        Distinct from a table because the numbers mean something different. A table cell is
        a value the engine interpolates between; an axis entry is a coordinate the
        interpolation is done ALONG. A tuner must be able to move the breakpoints, or the
        resolution of a fuel table is fixed by whoever wrote the firmware - which is the
        difference between a table you can shape and one you cannot.
        """
        return self.type == "axis1d"

    @property
    def is_enum(self) -> bool:
        return self.type == "enum"

    @property
    def is_bits(self) -> bool:
        return self.type == "bits"

    @property
    def is_float(self) -> bool:
        return self.type == "f32"

    def c_type(self) -> str:
        if self.is_table or self.is_axis:
            return SCALAR_TYPES[self.cell_type][0]
        if self.is_enum:
            return "uint8_t"
        if self.is_bits:
            return "uint8_t" if len(self.bits) <= 8 else "uint16_t"
        return SCALAR_TYPES[self.type][0]

    def storage_units(self) -> int:
        if self.is_table:
            return self.alloc_rows * self.alloc_cols * SCALAR_TYPES[self.cell_type][1]
        if self.is_axis:
            return self.alloc_count * SCALAR_TYPES[self.cell_type][1]
        if self.is_enum:
            return 1
        if self.is_bits:
            return 1 if len(self.bits) <= 8 else 2
        return SCALAR_TYPES[self.type][1]

    def alignment(self) -> int:
        """Natural alignment of this item's storage, which is NOT its size.

        A 16x16 u8 table is 256 bytes long but only 1 byte aligned, so laying it out
        on a 256-byte boundary would put every following offset in the wrong place
        and the C _Static_asserts would (correctly) fail.
        """
        if self.is_table or self.is_axis:
            return SCALAR_TYPES[self.cell_type][1]
        return C_ALIGNMENT[self.c_type()]

    def to_engineering(self, raw: float) -> float:
        return raw * self.scale

    def to_raw(self, engineering: float) -> float:
        return engineering / self.scale

    def cell_range(self) -> tuple[float, float]:
        if self.is_table or self.is_axis:
            return self.cell_min, self.cell_max
        return float(self.minimum), float(self.maximum)

    def enum_name(self, raw: int) -> str:
        for value in self.values:
            if value.value == raw:
                return value.name
        return f"INVALID({raw})"


def _exact_raw(item_type: str, engineering: float, scale: float, path: str) -> int | float:
    """Convert an engineering value to its raw storage form, refusing lossy results."""
    raw = engineering / scale
    if item_type == "f32":
        if not math.isfinite(raw):
            raise SchemaError(f"{path}: non-finite value {engineering!r}")
        return float(raw)
    rounded = round(raw)
    if abs(raw - rounded) > 1e-9:
        raise SchemaError(
            f"{path}: value {engineering} with scale {scale} is not representable in "
            f"{item_type} (raw would be {raw}); use an f32 field or a coarser scale"
        )
    return int(rounded)


def _parse_enum(raw: dict[str, Any], path: str) -> list[EnumValue]:
    spec = raw.get("values")
    values: list[EnumValue] = []
    if isinstance(spec, list):
        for entry in spec:
            if isinstance(entry, str):
                values.append(EnumValue(entry, len(values)))
            else:
                _require("name" in entry and "value" in entry, f"{path}: enum entry needs name+value")
                values.append(
                    EnumValue(str(entry["name"]), int(entry["value"]), " ".join(str(entry.get("description", "")).split()))
                )
    elif isinstance(spec, dict):
        for name, value in spec.items():
            _require(isinstance(value, int), f"{path}: enum value for {name} must be an int")
            values.append(EnumValue(str(name), int(value)))
    else:
        raise SchemaError(f"{path}: enum needs 'values' as a list or a mapping")
    _require(len(values) > 0, f"{path}: enum has no values")
    _require(len({v.value for v in values}) == len(values), f"{path}: duplicate enum values")
    _require(len({v.name for v in values}) == len(values), f"{path}: duplicate enum names")
    for value in values:
        # YAML 1.1 silently turns bare off/on/yes/no into booleans, which would smuggle a
        # non-identifier enum name into the firmware. Reject anything that is not a plain
        # lower-case identifier so this cannot happen again unnoticed.
        _require(
            value.name == value.name.lower() and value.name.replace("_", "").isalnum() and not value.name[0].isdigit(),
            f"{path}: enum name '{value.name}' must be a lower-case identifier; quote it in the YAML "
            "if it is a YAML boolean word such as off, on, yes or no",
        )
    return values


def _parse_item(raw: dict[str, Any], group: str, path: str) -> Item:
    for key in ("name", "id", "type"):
        _require(key in raw, f"{path}: missing required key '{key}'")
    name = str(raw["name"])
    item_id = int(raw["id"])
    item_type = str(raw["type"])
    _require(0 <= item_id <= 0xFFFF, f"{path}: id 0x{item_id:04X} is not 16-bit")

    item = Item(name=name, id=item_id, type=item_type, group=group)
    item.unit = str(raw.get("unit", ""))
    item.description = " ".join(str(raw.get("description", "")).split())
    item.scale = float(raw.get("scale", 1.0))
    item.owner = str(raw.get("owner", "TUNER")).upper()
    item.safety_class = str(raw.get("safety_class", "INFORMATIONAL")).upper()
    item.default_source = str(raw.get("default_source", "UNVERIFIED")).upper()
    item.source_ref = str(raw.get("source_ref", ""))
    item.live_edit = bool(raw.get("live_edit", True))
    item.derived = bool(raw.get("derived", False))
    deps = raw.get("dependencies", [])
    item.dependencies = [str(d) for d in deps] if isinstance(deps, list) else [str(deps)]

    _require(item.owner in OWNERS, f"{path}: owner '{item.owner}' not in {OWNERS}")
    _require(item.safety_class in SAFETY_CLASSES, f"{path}: safety_class '{item.safety_class}' invalid")
    _require(item.default_source in SOURCES, f"{path}: default_source '{item.default_source}' invalid")
    _require(item.scale > 0.0, f"{path}: scale must be positive")
    _require(
        not (item.owner == "READ_ONLY" and not item.derived),
        f"{path}: READ_ONLY items must be marked derived: true",
    )
    if item.default_source in PROVENANCE_REQUIRING_SOURCE:
        _require(bool(item.source_ref), f"{path}: {item.default_source} requires a source_ref")

    if item.is_enum:
        item.values = _parse_enum(raw, path)
        item.minimum, item.maximum, item.default = 0, len(item.values) - 1, int(raw.get("default", 0))
        item.raw_minimum, item.raw_maximum = 0, len(item.values) - 1
        item.raw_default = int(item.default)
        _require(0 <= item.raw_default < len(item.values), f"{path}: default {item.default} is not a valid enum value")
        return item

    if item.is_bits:
        item.bits = [str(b) for b in raw.get("bits", [])]
        _require(len(item.bits) > 0, f"{path}: bits item has no bits")
        _require(len(set(item.bits)) == len(item.bits), f"{path}: duplicate bit names")
        _require(len(item.bits) <= 16, f"{path}: at most 16 bits are supported")
        # A bitfield's engineering range is 0 .. 2^n - 1, not 0..0: a default that sets a
        # bit would otherwise be reported as out of range by the item validator.
        item.minimum = 0
        item.maximum = (1 << len(item.bits)) - 1
        item.default = int(raw.get("default", 0))
        item.raw_minimum, item.raw_maximum = 0, item.maximum
        item.raw_default = int(item.default)
        _require(0 <= item.raw_default <= item.maximum, f"{path}: bit default out of range")
        return item

    if item_type == "axis1d":
        item.alloc_count = int(raw["alloc_count"])
        item.cell_type = str(raw.get("cell_type", "u16"))
        item.cell_min = float(raw.get("cell_min", 0.0))
        item.cell_max = float(raw.get("cell_max", 1000.0))
        _require(2 <= item.alloc_count <= 64,
                 f"{path}: alloc_count must be 2..64 (an axis with one breakpoint "
                 f"interpolates to a constant and is a mistake, not a tuning choice)")
        _require(item.cell_type in SCALAR_TYPES, f"{path}: unknown cell_type '{item.cell_type}'")
        _require(item.cell_max > item.cell_min, f"{path}: cell_max must exceed cell_min")
        lo, hi = SCALAR_TYPES[item.cell_type][2:]
        _require(
            lo <= item.cell_min and item.cell_max <= hi,
            f"{path}: axis range [{item.cell_min},{item.cell_max}] escapes the "
            f"{item.cell_type} domain",
        )
        item.row_axis = str(raw.get("row_axis", ""))
        item.minimum, item.maximum = item.cell_min, item.cell_max
        item.default = item.cell_min
        item.raw_minimum, item.raw_maximum = item.cell_min, item.cell_max
        item.raw_default = item.cell_min
        # The fill is the FIRST breakpoint; the generator lays the rest out evenly across
        # the declared range, which is the right starting point for an axis and not a
        # claim about it. A tuner moves them.
        item.default_fill = float(raw.get("axis_first", item.cell_min))
        _require(
            item.cell_min <= item.default_fill <= item.cell_max,
            f"{path}: axis_first escapes the axis range",
        )
        return item

    if item_type == "table2d":
        item.alloc_rows = int(raw["alloc_rows"])
        item.alloc_cols = int(raw["alloc_cols"])
        item.row_axis = str(raw["row_axis"])
        item.col_axis = str(raw["col_axis"])
        item.cell_type = str(raw.get("cell_type", "u8"))
        item.cell_min = float(raw.get("cell_min", 0.0))
        item.cell_max = float(raw.get("cell_max", 255.0))
        _require(1 <= item.alloc_rows <= 64, f"{path}: alloc_rows must be 1..64")
        _require(1 <= item.alloc_cols <= 64, f"{path}: alloc_cols must be 1..64")
        _require(item.cell_type in SCALAR_TYPES, f"{path}: unknown cell_type '{item.cell_type}'")
        _require(item.row_axis and item.col_axis, f"{path}: table2d needs row_axis and col_axis")
        _require(item.cell_max > item.cell_min, f"{path}: cell_max must exceed cell_min")
        lo, hi = SCALAR_TYPES[item.cell_type][2:]
        _require(
            lo <= item.cell_min and item.cell_max <= hi,
            f"{path}: cell range [{item.cell_min},{item.cell_max}] escapes the {item.cell_type} domain",
        )
        item.minimum, item.maximum, item.default = item.cell_min, item.cell_max, 0
        item.raw_minimum, item.raw_maximum = item.cell_min, item.cell_max
        item.raw_default = 0
        # A table is initialised to a declared fill rather than to zero.  A zero-filled
        # calibration table is not a conservative default, it is a broken one: it
        # commands no air and therefore no fuel.
        # The fill is kept in ENGINEERING units and as a float: an int field would silently
        # truncate a 0.90 lambda target to 0 and ship a table that is out of its own range.
        item.default_fill = float(raw.get("default_fill", 0.0))
        cell_lo, cell_hi = SCALAR_TYPES[item.cell_type][2:]
        _require(
            cell_lo <= item.default_fill <= cell_hi,
            f"{path}: default_fill {item.default_fill} escapes the {item.cell_type} domain "
            f"[{cell_lo}, {cell_hi}]",
        )
        # If zero is not a legal cell, the table MUST declare a fill.  A table that ships
        # zero-filled while its own range forbids zero is a broken calibration: it commands
        # a value the ECU itself considers invalid, and it does so on every cold start.
        if cell_lo > 0 and "default_fill" not in raw:
            _require(
                False,
                f"{path}: {item.name} has cell_min {cell_lo} but no default_fill, so it would "
                f"ship zero-filled and immediately violate its own range",
            )
        return item

    _require(item_type in SCALAR_TYPES, f"{path}: unknown type '{item_type}'")
    ctype, _, domain_lo, domain_hi = SCALAR_TYPES[item_type]
    item.minimum = raw.get("min", domain_lo if item_type != "f32" else -1.0e6)
    item.maximum = raw.get("max", domain_hi if item_type != "f32" else 1.0e6)
    item.default = raw.get("default", 0)
    for label, value in (("min", item.minimum), ("max", item.maximum), ("default", item.default)):
        _require(
            isinstance(value, (int, float)) and not isinstance(value, bool),
            f"{path}: {label} must be numeric, got {value!r}",
        )
    if item_type == "bool":
        item.minimum, item.maximum = 0, 1
        _require(item.default in (0, 1, False, True), f"{path}: bool default must be 0 or 1")
        item.default = int(bool(item.default))
    _require(float(item.minimum) <= float(item.maximum), f"{path}: min > max")

    if item_type == "f32":
        # f32 stores the engineering value directly unless a scale is declared.
        for label, value in (("min", item.minimum), ("max", item.maximum), ("default", item.default)):
            _require(math.isfinite(float(value)), f"{path}: {label} must be finite")
        item.raw_minimum = float(item.minimum) / item.scale
        item.raw_maximum = float(item.maximum) / item.scale
        item.raw_default = float(item.default) / item.scale
    else:
        item.raw_minimum = _exact_raw(item_type, float(item.minimum), item.scale, f"{path}.min")
        item.raw_maximum = _exact_raw(item_type, float(item.maximum), item.scale, f"{path}.max")
        item.raw_default = _exact_raw(item_type, float(item.default), item.scale, f"{path}.default")
        # The *raw* bounds still have to fit the storage type.
        for label, raw in (("min", item.raw_minimum), ("max", item.raw_maximum), ("default", item.raw_default)):
            _require(
                domain_lo <= raw <= domain_hi,
                f"{path}: raw {label} {raw} escapes the {item_type} domain "
                f"[{domain_lo:g},{domain_hi:g}] - adjust the scale or widen the type",
            )
        item.raw_minimum = int(item.raw_minimum)
        item.raw_maximum = int(item.raw_maximum)
        item.raw_default = int(item.raw_default)
    return item


def _assign_offsets(items: list[Item]) -> int:
    """Lay items out in order with natural alignment, exactly as a C compiler would.

    Returns the total image size.  The result is re-proved in C by
    ``_Static_assert(offsetof(...))`` in the generated header.
    """
    offset = 0
    for item in items:
        units = item.storage_units()
        align = item.alignment()
        if align > 1 and offset % align != 0:
            offset += align - (offset % align)
        item.offset = offset
        offset += units
    if offset % 4 != 0:
        offset += 4 - (offset % 4)
    return offset


@dataclass
class Schema:
    version: int
    groups: list[str]
    items: dict[str, Item]
    order: list[str]
    total_size: int
    source_files: list[str]

    def by_id(self, item_id: int) -> Item | None:
        for item in self.items.values():
            if item.id == item_id:
                return item
        return None

    def group_items(self, group: str) -> list[Item]:
        return [self.items[name] for name in self.order if self.items[name].group == group]


def load_schema(schema_dir: Path) -> Schema:
    """Parse, validate and lay out every item found in ``schema_dir``."""
    files = sorted(Path(schema_dir).glob("*.yaml"))
    _require(bool(files), f"no schema files found in {schema_dir}")

    groups: list[str] = []
    items: dict[str, Item] = {}
    source_files: list[str] = []

    for path in files:
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        _require(isinstance(doc, dict), f"{path.name}: top level must be a mapping")
        version = int(doc.get("schema_version", SCHEMA_VERSION))
        _require(
            version == SCHEMA_VERSION,
            f"{path.name}: schema_version must be {SCHEMA_VERSION} (found {version})",
        )
        source_files.append(path.name)

        for group_raw in doc.get("groups", []):
            gname = str(group_raw["name"])
            gbase = int(group_raw["base_id"])
            _require(gname not in groups, f"{path.name}: duplicate group '{gname}'")
            _require(0x0000 <= gbase <= 0xFF00, f"{gname}: base_id out of range")
            groups.append(gname)
            for entry in group_raw.get("items", []):
                ipath = f"{path.name}:{gname}.{entry.get('name', '?')}"
                item = _parse_item(entry, gname, ipath)
                _require(
                    gbase <= item.id <= gbase + 0xFF,
                    f"{ipath}: id 0x{item.id:04X} outside group base 0x{gbase:04X}",
                )
                _require(item.name not in items, f"{ipath}: duplicate item name '{item.name}'")
                for other in items.values():
                    _require(other.id != item.id, f"{ipath}: id 0x{item.id:04X} already used by '{other.name}'")
                items[item.name] = item

    for item in items.values():
        for dep in item.dependencies:
            _require(dep in items, f"{item.name}: dependency '{dep}' does not exist")

    ordered = sorted(items.values(), key=lambda it: (groups.index(it.group), it.id))
    total = _assign_offsets(ordered)
    return Schema(
        version=SCHEMA_VERSION,
        groups=groups,
        items=items,
        order=[it.name for it in ordered],
        total_size=total,
        source_files=source_files,
    )


def validate_defaults(schema: Schema) -> list[str]:
    """Return human-readable problems with the shipped defaults.

    Integer items additionally get a compile-time proof in the generated C; this
    function catches both integer and float problems before C is generated, with a
    message a human can act on.
    """
    problems: list[str] = []
    for name in schema.order:
        item = schema.items[name]
        if item.is_table or item.is_enum or item.is_bits:
            continue
        value = float(item.default)
        if value < float(item.minimum) or value > float(item.maximum):
            problems.append(f"{name}: default {item.default} outside bounds [{item.minimum}, {item.maximum}]")
    return problems


def fmt_number(value: Any) -> str:
    """Format a number as a C literal, without a type suffix."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise SchemaError(f"non-finite literal {value!r}")
        if value == int(value) and abs(value) < 1e15:
            return f"{int(value)}.0"
        return repr(value)
    raise SchemaError(f"unsupported literal {value!r}")
