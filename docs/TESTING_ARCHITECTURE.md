# TESTING ARCHITECTURE

**Document ID:** TA-001 · **Status:** PHASE 1

How tests are *built and run* — the infrastructure that makes §88–§91 achievable rather
than aspirational.

---

## 1. The central idea: one codebase, several back ends

```
                      ┌──────────────────────────────────────┐
                      │  portable engine control (C++17)     │
                      │  firmware/core … firmware/diagnostics │
                      └───────────────┬──────────────────────┘
                                      │  raccoon_hal_t interface
          ┌───────────────────────────┼────────────────────────────┐
          ▼                           ▼                            ▼
   ┌─────────────┐            ┌──────────────┐             ┌─────────────┐
   │ target HAL  │            │ virtual HAL  │             │ HIL bridge  │
   │ (STM32)     │            │ (host)       │             │ (bench)     │
   └──────┬──────┘            └──────┬───────┘             └──────┬──────┘
          │                          │                            │
     real ECU              unit tests · SIL · simulator      real ECU under
                                                       bench stimulus
```

The tests exercise **the shipping engine code**. A test that reimplements the logic it is
testing is not a test of the product. The virtual HAL is not a mock of the ECU; it is the
ECU's only portable dependency.

---

## 2. Test layers and their runners

| Layer | Runner | Trigger | Coverage target |
|---|---|---|---|
| Host unit | `ctest` / CTest + a lightweight assertion framework | every push | ≥ 90 % line, 100 % branch on `scheduler`, `safety`, `torque` |
| Property/fuzz | libFuzzer-style entry points + a deterministic PRNG fuzz driver | every push (bounded time) | invariants, not coverage |
| Sanitizers | ASan + UBSan builds of the whole unit suite | every push | zero reports |
| Python unit | `pytest` | every push | ≥ 85 % |
| Python static | `ruff`, `mypy`, `bandit` | every push | zero |
| Coverage gates | coverage report diff | every push | no decrease on the portable core |
| Layer check | `tools/analysis/layer_check.py` | every push | no forbidden includes/imports |
| Schema codegen | regenerate + diff | every push | generated files must match |
| Default bounds | compile-time assertions | every push | a bad default fails the build |
| Licence scan | `tools/analysis/license_scan.py` | every push | no GPL/unknown dependency |
| HIL | bench script | on demand, per release | recorded report |
| EOL | bench script | per unit | machine-readable report |

---

## 3. Deterministic testing

Timing tests are only meaningful if they are repeatable.

- All randomness uses a **seeded, explicit PRNG**; a failing test prints its seed, and the
  seed reproduces the failure exactly.
- Virtual-HAL time is **logical**: the test advances time explicitly, so a test never
  depends on wall-clock speed and a slow machine does not cause flakes.
- Fault injection is scripted and deterministic: "drop the 7th tooth after 12 revolutions
  at 3400 rpm" produces the same result every run.
- No sleeps in unit tests.

---

## 4. Test structure

```
tests/
  unit/            module tests, mirroring the source tree
  property/        invariants: scheduler, safety envelope, torque arbitration
  fuzz/            target entry points + corpora
  sil/             whole-system scenarios with the plant model
  matrix/          1–4 cyl × strategies × fuel/ignition paths
  fixtures/        shared: recorded edge streams, synthetic sensor data, tables
  golden/          reference outputs (map interpolation, protocol bytes, log frames)
  hil/             bench scripts, stimulus generators, analysis
  eol/             factory test definition and report schema
  data/            measured datasets (once they exist — with provenance)
tuner/tests/
  unit/  integration/  gui/  golden/
```

**Golden files** are used where exact output matters (protocol encoding, log format, map
interpolation, CRC). A golden mismatch is a deliberate, reviewed change — never an
auto-updated expectation.

---

## 5. Invariants tested as properties

These are the assertions that must hold for *all* inputs, not just tested ones:

| Invariant | Property test |
|---|---|
| No event fires twice | for any event stream, the number of executed events ≤ the number of created events per generation |
| No stale event executes | for any plan revision sequence, no event with an old generation ever executes |
| No cut is resurrected | for any sequence of cut/clear requests, a cut is cleared only by its owner |
| Hard limits always win | for any combination of requests within their bounds, the final command is within the envelope |
| Fuel cut is final | for any enrichment/min-pulse/closed-loop request during a cut, output is zero |
| Dwell is bounded | for any battery/rpm/table combination, dwell ∈ [min, max] |
| Injector pulse is bounded | as above |
| Fuel mass is finite and non-negative | for any sensor combination within validity bounds |
| Control is allocation-free and bounded | a static check plus a runtime allocation counter that must read zero in the control loops |
| Nothing unbounded | a static check for unbounded loop patterns in the real-time region |

---

## 6. The plant model (SIL)

A simple, transparent model is deliberately chosen over a complex one: a SIL model whose
physics is wrong will mislead, while a simple model exposes integration problems.

```
engine:     inertia + torque from a simple air/fuel combustion term
crankshaft: tooth generation from rpm, with a configurable wheel
sensors:    transfer functions + noise + configurable faults
combustion: first-order mixture transport → lambda (crude, adequate for closed-loop tests)
thermal:    lumped masses for ECT, IAT, oil
vehicle:    mass + drag + gearing + tyre slip (for TC tests)
```

Its purpose is **not** to predict power. It is to prove that the *control loops* are stable,
bounded, and recoverable.

---

## 7. Test data provenance

Every dataset in `tests/data/` carries a manifest entry:

```yaml
file: gt650_dyno_run_2026_11_04.csv
provenance: DYNO-MEASURED
source: "run log, dyno cell 2, 2026-11-04"
calibration_hash: sha256:...
firmware: <build hash>
notes: "MAP 3.1 bar, ambient 31 C, no wideband — lambda from OEM narrowband only"
```

A dataset with no provenance is not used for any claim. This enforces §124 at the data
level.

---

## 8. CI pipeline

```
push ──▶ build (host, debug) ──▶ unit ──▶ sanitizers ──▶ property/fuzz
     ├─▶ build (target) ──▶ size/budget check ──▶ static analysis ──▶ codegen diff
     ├─▶ python: ruff, mypy, pytest, coverage, bandit
     ├─▶ layer check ──▶ licence scan ──▶ release-build dry run (dev flags must fail)
     └─▶ merge only if every gate is green
```

Nightly additionally runs: extended fuzzing, a 2-hour SIL endurance, and a memory-growth
check. Release additionally requires: a full HIL report, an EOL dry run, and an updated
`UNKNOWN_REGISTER` diff review.

---

## 9. Coverage philosophy

Coverage is a *means*, not a target. The critical standard is that the **real-time region,
the safety envelope, the scheduler invariants, and the arbitration table** are covered
exhaustively by invariants and property tests, not by line count. A line in the torque
manager that is "covered" by a test that never asserts the winner is not covered in any
meaningful sense.
