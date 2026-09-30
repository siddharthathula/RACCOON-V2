# CALIBRATION SPECIFICATION

**Document ID:** CAL-001 · **Status:** PHASE 1

Calibration is not a bag of variables (spec §55). It is a **typed, validated, versioned,
transactional, provenance-tagged database**.

---

## 1. Item definition

Every calibration item, scalar or table cell, carries:

```yaml
id: 0x1204                      # stable 16-bit ID, never positional
name: ignition.advance
type: table_2d | scalar_u8 | scalar_s16 | curve | bitset | bytes
unit: deg
description: >
  Base ignition advance, rpm × load. The final command is this value plus all
  corrections, clamped by the hard safety envelope.
range: {min: -20.0, max: 60.0}  # hard bounds, enforced by the ECU, not the GUI
default: 10.0
default_source: SOURCE-DERIVED   # or CALCULATED | MEASURED | SIMULATED | ...
source_ref: "GT650-001 §1"      # document, or a measurement record ID
validation: {axis_monotonic: true, min_cells: 8x8, max_cells: 32x32}

# An axis1d item holds the BREAKPOINTS for one dimension of one table. It is a separate
# item, not a field on the table, because it is separately tunable, separately validated
# and separately owned - and because a host that treats it as either a scalar or a table
# indexes the wrong number of things and writes an image of exactly the right length
# holding entirely wrong values.
#
#   - name: ve_table_rpm_axis
#     type: axis1d
#     alloc_count: 16        # one breakpoint per row of ve_table
#     cell_type: u16         # rpm runs past 65,535 nowhere, but a byte cannot hold 12,000
#     cell_min: 1000         # the range a breakpoint may take
#     cell_max: 12000
#     axis_first: 1000       # the first breakpoint; the rest are laid out evenly from here
#     unit: rpm
#
# The descriptor resolves each table dimension to the id of its axis item
# (row_axis_id / col_axis_id). row_axis names only the axis KIND - "rpm", "kPa" - and
# resolves to no item, so a host matching on it alone is guessing.
#
# Ordering is a cross-item rule, not a per-item bound, because it is a relationship between
# two breakpoints rather than a property of one. It is checked at activation: a per-point
# neighbour check on write would make it impossible to drag a breakpoint past the one
# beside it, which is the edit a tuner makes most often.
dependencies: [engine.cylinder_count, safety.ign_max]
owner: TUNER                     # READ_ONLY | ENGINEERING | TUNER | SAFETY | MODE
safety_class: HIGH               # INFORMATIONAL | MEDIUM | HIGH | CRITICAL
unit_test: FUEL-UNIT-014         # the test that guards this item
```

**Rules:**
- A `SAFETY`-class item may only be *lowered* by engineering mode; it can never be raised
  beyond a compiled ceiling.
- `READ_ONLY` items are derived; the GUI shows them read-only and explains their derivation.
- A `default` with `default_source: UNVERIFIED` causes the ECU to raise an informational
  DTC when the item is used outside a bench/dyno context. This is how "unmeasured" is kept
  visible without blocking the user.

---

## 2. Storage layout and transactions (§56)

```
Slot A  ┌─────────────────────────────┐
        │ image header                │  version, length, CRC, signature
        │ block table                 │  per-block CRC
        │ payload (schema-versioned)  │
Slot B  │ (the previous committed image)                 │
Commit  └─────────────────────────────┘
        ┌─────────────────────────────┐  ← commit record in a DIFFERENT flash page
        │ committed_slot, image CRC,  │     with its own CRC
        │ schema version, timestamp   │
        └─────────────────────────────┘
```

**Write sequence (spec §56 verbatim order):**

```
READ     → read the current image, verify CRC, report its version
VALIDATE → parse against the schema; check types, ranges, dependencies, safety class
STAGE    → write the new image into the INACTIVE slot
CRC      → verify the written image byte-for-byte
COMMIT   → write the commit record (the atomic step)
VERIFY   → read back the committed image and compare
ACTIVATE → switch the live pointer
```

**Interruption at any step** → the commit record still points at the old slot ⇒ the old
image runs. Power loss during `STAGE` or `CRC` is harmless. There is no state in which a
partial configuration is observable.

**Rollback**: an explicit, authenticated command re-commits the previous image. Combined
with the A/B layout, the ECU always has a known-good configuration.

**Live (tuner) writes** to non-safety items may be applied immediately in RAM with a
"dirty" flag; the slot is rewritten on an explicit save or on a debounce timer, so a crash
cannot lose a session and cannot corrupt a slot.

---

## 3. Mode overlays (§48)

A tune = **one base image + one active mode overlay**. Modes are stored as overlays so
switching modes does not require rewriting the base, and so a mode change is itself a small
validated transaction. Hard safety limits live in the **base** image and are therefore
**global** (§48).

---

## 4. Version control (§57)

```
TuneVersion {
  id, name, parent, timestamp, user, description
  ecu_serial, hardware_profile, firmware_build, schema_version
  engine_profile, fuel_type
  content_hash, parent_hash
}
```

Operations: save · duplicate · compare · rollback · branch · merge · export/import.

**Compare** produces a structural diff (items changed, min/max delta, table heatmap
difference, histogram) — the same machinery the map editor uses (§72).

**Merge** is a per-cell three-way merge with an explicit conflict list. A merge is never
silent: every conflicting cell is presented with base/ours/theirs and requires a decision.
**No auto-resolution of safety-class items** — a merged safety item is always flagged as
requiring explicit confirmation.

---

## 5. Schema and code generation (§100, §102)

```
configs/schema/*.yaml          ← single source of truth
        │
        ├── tools/codegen → firmware/generated/cal_types.h + cal_defaults.c
        ├── tools/codegen → tuner/generated/cal_types.py
        ├── tools/codegen → docs/CALIBRATION_REFERENCE.md  (auto-generated)
        └── tools/codegen → a JSON descriptor the GUI renders forms from
```

- Generated files are committed **and** reproducible: CI regenerates and diffs. A mismatch
  fails the build.
- `tools/codegen` also emits a **static-assertion table**: for every safety-class item, a
  compile-time check that the shipped default is inside `[min, max]`. A default that
  violates a bound is a build failure, not a runtime surprise.

---

## 6. Validation rules (spec §97, §98)

Before a configuration may be written or the engine armed:

| Check | Example failure message |
|---|---|
| Schema/type/range | `ignition.advance: value 85 exceeds max 60` |
| Axis monotonicity | `axis breakpoints must strictly increase; a repeated or inverted pair makes the table it belongs to unsearchable` (rule `axis_breakpoints_increase`, reported against the offending axis item and the index of the first bad pair) |
| Table size within limits | `boost_target: 40×40 exceeds the 32×32 limit for this profile` |
| Dependency | `injection=SEQUENTIAL requires cam sync; engine profile has cam_channels=0` |
| Capability | `knock control requires CAP_KNOCK; hardware profile lacks it` |
| Assignment conflicts | `output 7 is assigned to both fuel_pump and vvt` |
| Trigger plausibility | `trigger angle 640° is outside [0,360)` |
| Firing order | `firing_order must be a permutation of 0..cylinder_count−1` |
| Cycle consistency | `sequential injection on a 2-stroke configuration is invalid` |
| Safety completeness | `every CRITICAL safety item must be set` |
| Cross-sensor | `tps2 assigned but the profile has only one TPS input` |
| Physical plausibility | `idle target 1400 rpm exceeds the rev-limiter start` |
| CRC / signature | `image CRC mismatch — configuration rejected` |

A wizard (§97) runs exactly these validators and **cannot produce an unsafe configuration
silently**: every refusal names the rule and the remedy.

---

## 7. Arming gate (§98)

```
ENGINE CONTROL NOT ARMED  unless ALL of:
  ✓ trigger configured and a sync strategy exists for the mode
  ✓ TDC / firing order confirmed (validated flag, set by the TDC tool or bench procedure)
  ✓ injector and ignition assignment complete and conflict-free
  ✓ sensor configuration valid (required sensors for the chosen strategies present)
  ✓ calibration CRC + signature valid
  ✓ every hard limit set and within its compiled ceiling
  ✓ no actuator conflict
  ✓ kill-switch state permits
  ✓ hardware capabilities satisfy the requested features
```

The gate is evaluated in the ECU, not only in the GUI, and its result is a telemetry field
so a dyno operator can see *why* the engine will not arm.

---

## 8. Live vs stored

| Category | Live (RAM) | Stored (flash) |
|---|---|---|
| Map cell edits in tuning mode | yes | on save / debounce |
| Scalars marked `live_edit` | yes | on save |
| Safety limits | no — write requires engineering mode + commit | on commit |
| Derived values | recomputed | never stored |

---

## 9. Test matrix

| ID | Test | Pass condition |
|---|---|---|
| C1 | Round-trip | write → read-back → identical, for every item and every table |
| C2 | Bounds | out-of-range rejected on the ECU, not just the GUI |
| C3 | Dependency refusal | invalid combination rejected with a specific reason |
| C4 | Power loss at each transaction step | old image always runs; no partial image ever observed (fault injection ×7 steps) |
| C5 | CRC corruption | detected; refuses to run; offers the previous slot |
| C6 | Signature invalid (production build) | refuses to activate; logs an audit entry |
| C7 | Schema version mismatch | negotiated downgrade/upgrade path; no misinterpretation |
| C8 | Version control | branch → modify → merge → conflict list correct; safety items always flagged |
| C9 | Rollback | previous image restored and verified |
| C10 | Default-vs-bounds build check | a bad default fails the build |
| C11 | Arming gate | each individual missing condition produces `NOT ARMED` with the reason |
| C12 | Mode overlay | switching modes changes only overlay values; safety limits unchanged |
