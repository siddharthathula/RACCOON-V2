#!/usr/bin/env python3
"""RACCOON V2 calibration code generator.

Usage:
    python3 tools/codegen/generate.py              # write all outputs
    python3 tools/codegen/generate.py --check      # fail if outputs are stale (CI gate)
    python3 tools/codegen/generate.py --print-size # just report the image size

Outputs (all deterministic):
    firmware/generated/cal_types.h
    firmware/generated/cal_defaults.c
    firmware/generated/cal_descriptors.c
    firmware/generated/cal_access.h
    tuner/generated/cal_ids.py
    configs/calibration_descriptor.json
    docs/CALIBRATION_REFERENCE.md

A schema error, a duplicate id, a dependency on a missing item, or a default outside
its own bounds is a hard failure.  That is deliberate: the whole point of the
single-source-of-truth model is that a mistake cannot reach the firmware.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from rc_emit import (  # noqa: E402
    emit_c_access,
    emit_c_rules,
    emit_c_defaults,
    emit_c_descriptors,
    emit_c_header,
    emit_docs,
    emit_json,
    emit_python,
)
from rc_rules import RULES, validate_rules  # noqa: E402
from rc_schema import SchemaError, load_schema, validate_defaults  # noqa: E402

SCHEMA_DIR = ROOT / "configs" / "schema"

TARGETS = (
    ("firmware/generated/cal_types.h", emit_c_header),
    ("firmware/generated/cal_defaults.c", emit_c_defaults),
    ("firmware/generated/cal_descriptors.c", emit_c_descriptors),
    ("firmware/generated/cal_access.h", emit_c_access),
    ("firmware/generated/cal_rules.h", emit_c_rules),
    ("tuner/generated/cal_ids.py", emit_python),
    ("configs/calibration_descriptor.json", emit_json),
    ("docs/CALIBRATION_REFERENCE.md", emit_docs),
)

HEADER_NOTE = (
    "# Generated file - do not edit.\n"
    "# The source of truth is the schema under configs/schema/.\n"
    "# Regenerate with:  python3 tools/codegen/generate.py\n"
)


def render_all() -> dict[str, str]:
    schema = load_schema(SCHEMA_DIR)
    problems = validate_defaults(schema) + validate_rules(schema)
    if problems:
        raise SchemaError(
            "schema failed validation:\n  " + "\n  ".join(problems)
        )
    rendered: dict[str, str] = {}
    for rel, fn in TARGETS:
        text = fn(schema)
        if rel.endswith(".py"):
            text = HEADER_NOTE + text
        rendered[rel] = text
    return rendered


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="verify outputs are up to date")
    ap.add_argument("--print-size", action="store_true", help="print the image size and exit")
    args = ap.parse_args()

    try:
        schema = load_schema(SCHEMA_DIR)
    except SchemaError as exc:
        print(f"schema error: {exc}", file=sys.stderr)
        return 2

    problems = validate_defaults(schema) + validate_rules(schema)
    if problems:
        print("schema failed validation:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 2

    if args.print_size:
        print(
            f"schema v{schema.version}: {len(schema.order)} items, {schema.total_size} bytes, "
            f"{len(RULES)} cross-item rules all satisfied"
        )
        return 0

    try:
        outputs = render_all()
    except SchemaError as exc:
        print(f"codegen error: {exc}", file=sys.stderr)
        return 2

    if args.check:
        stale: list[str] = []
        for rel, text in outputs.items():
            path = ROOT / rel
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(rel)
        if stale:
            print("generated files are stale, run tools/codegen/generate.py:", file=sys.stderr)
            for rel in stale:
                print(f"  {rel}", file=sys.stderr)
            return 1
        print(f"generated files are up to date ({len(outputs)} files, {schema.total_size} byte image)")
        return 0

    for rel, text in outputs.items():
        path = ROOT / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        print(f"wrote {rel}")
    print(f"image size: {schema.total_size} bytes, {len(schema.order)} items")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
