# TEST PLAN

**Document ID:** TP-001 · **Status:** PHASE 1 — the ladder every phase must climb

---

## 1. Ladder

```
 UNIT ──▶ INTEGRATION ──▶ SIL ──▶ HIL ──▶ BENCH ──▶ ENGINE ──▶ DYNO ──▶ VEHICLE ──▶ ENDURANCE
 (CI)       (CI)          (CI+PC)   (bench)   (bench)   (stand)   (dyno)   (track/strip)
```

Rule: **no phase is declared complete until the level below it is green**, and no
performance, safety, or real-time claim is made from a level that has not measured it
(§91, §114, §124).

---

## 2. Verification labels used throughout this project

| Label | Meaning |
|---|---|
| `VERIFIED` | proven by an automated test that runs in CI |
| `PARTIALLY VERIFIED` | some paths covered; the gaps are listed |
| `MEASURED` | measured on hardware, with the method and conditions recorded |
| `HIL VERIFIED` | measured on a real ECU under HIL |
| `ENGINE VERIFIED` | observed on a running engine |
| `DYNO VERIFIED` | measured on a dyno |
| `VEHICLE VALIDATED` | observed in vehicle use |
| `UNKNOWN` | not established; must be in `UNKNOWN_REGISTER.md` |
| `BLOCKED` | cannot be established yet; the blocker is stated |

---

## 3. Unit tests (CI, every commit)

| Area | Content | Tooling |
|---|---|---|
| Trigger decoder | every profile; every state transition as an exact sequence; noise/false/missing tooth; stall/restart; reversal; overspeed | host build, ASan+UBSan |
| Engine angle | period→RPM, prediction, wrap safety, cycle counting | host |
| Scheduler | queue invariants, generation guard, no-resurrection, deadline expiry, collision | host |
| Sensor framework | all fault codes, freshness, rate limiting, transfer functions (endpoints + mid) | host |
| Fuel | dead time, min/max pulse, battery/pressure compensation, transient bounds, cut precedence, closed-loop stability | host |
| Ignition | dwell bounds, angle clamping, correction summation order, per-cylinder | host |
| Knock | window, envelope, recovery bounds, escalation | host |
| Torque manager | the full precedence table, every pair, plus fuzz (`UNK-FW-012`) | host, property-based |
| Safety envelope | every hard limit wins over every requester (property test over the request space) | host, property-based |
| Calibration | bounds, dependencies, transactions, CRC, rollback | host |
| Protocol | framing, CRC, fragmentation, replay, auth, role enforcement | host |
| Logging | encode/decode round trip, truncation, corruption | host |
| CAN database | decode of every field type, validation refusals | host |

**Standard:** sanitizers on, `-Werror`, coverage recorded. A PR lowers no coverage on
`firmware/core`…`firmware/diagnostics`.

---

## 4. Integration tests (CI)

- `trigger → scheduler → injector` and `trigger → scheduler → ignition` end-to-end, with
  the virtual HAL, asserting the *actual* output edges.
- `sensors → load → fuel → safety` chain.
- `torque manager → safety → scheduler` under simultaneous requesters.
- `calibration transaction → commit → activation → engine behaviour change`.
- `protocol → calibration → firmware update` over each transport.

---

## 5. SIL (host, whole engine core + plant model)

A plant model provides: crank/cam edges from an engine model, sensor responses, lambda
from a simple combustion/transport model, thermal mass, and vehicle dynamics.

| Test | Purpose |
|---|---|
| Closed-loop stability at every cylinder count | find the stability margin (`UNK-FW-010`) |
| Transient response | acceleration enrichment adequacy; no overshoot beyond bounds |
| 8 h endurance at the maximum load | memory, arithmetic, state-machine stability |
| Fault injection, all entries | deterministic recovery (`UNK-FW-012`) |
| Randomised fuzzing of the event stream | the trigger/scheduler must never produce an invalid schedule or a stale event |
| 1/2/3/4 cylinder × strategies | the multi-cylinder matrix (§89) |
| Fuzz of simultaneous features | launch + TC + shift + knock + boost, 10⁶ cases |

---

## 6. HIL — see [`HIL_SPEC.md`](HIL_SPEC.md)

---

## 7. Bench

- Board bring-up: power rails, ADC accuracy per channel, digital input thresholds, timer
  accuracy, CAN bitrate tolerance, USB throughput, storage endurance, watchdog.
- Protection: ISO 7637-2 pulses, reverse polarity, load dump, brownout behaviour.
- **End-of-line test (§99)** — automated, machine-readable report per unit:

| Test | Pass condition |
|---|---|
| Power rails | within tolerance at 6/12/18 V |
| ADC per channel | within tolerance against a precision source |
| Digital inputs | threshold and debounce correct |
| CKP / CMP | both channels capture correctly at a reference frequency |
| Injector drivers | current limit, feedback, and a commanded pulse verified on a scope |
| Ignition drivers | commanded dwell and a spark verified (or coil current, in a safe fixture) |
| CAN | transmit, receive, and bit-error detection at 500 kbit/s |
| USB | enumeration, sustained throughput, no control-loop perturbation |
| Storage | write/read/erase, endurance smoke |
| Watchdog | forced trip produces a reset with the correct cause |
| Calibration | write → read-back → CRC |
| Outputs | each output switched and measured; the default state verified |
| **Report** | JSON with serial number, board revision, firmware hash, every result, and a pass/fail signature |

A unit that fails EOL is not shipped, and the report is retained for traceability.

---

## 8. Engine test (stand, then dyno)

Sequence, each step gated on the previous:

1. **Cold start, no fuel** — verify sync, RPM, cam phase, and that nothing fires.
2. **Firing verification** — TDC strobe, firing order, dwell, spark, injector pulse; the
   timing light and the ECU must agree.
3. **First start with fuel** — cranking enrichment; base timing.
4. **Idle** — closed loop, idle control.
5. **Progressive drive** — build base calibration slowly, logging every step.
6. **WOT pull** — lambda targets, VE, ignition; **protection verified by induced faults**
   (e.g. an unplugged sensor must produce the designed action).
7. **Knock search** (if a sensor is fitted) — establish the real limit by measurement.
8. **Thermal soak** — verify oil/coolant temperature protection by observation.
9. **Dyno** — establish and *measure* power/torque; produce the baseline run set.

Nothing about power, torque, AFR, or RPM is claimed before this ladder is complete.

---

## 9. GT650 bench characterisation (unblocks the application profile)

The measurements in `GT650_RESEARCH.md` that only hardware can answer:

| # | Measurement | Method |
|---|---|---|
| 1 | Crank wheel tooth count and gap layout | Photograph + count; scope of raw CKP |
| 2 | Cam signal presence, tooth count, phase | Scope + cam observation at TDC |
| 3 | Firing order and cylinder numbering | Strobe + piston position |
| 4 | Trigger angle at TDC | Strobe at a known rpm against the ECU's reported angle |
| 5 | Injector part number, coil resistance, flow | DMM + flow test at 3 and 4.5 bar |
| 6 | Fuel pressure at idle/cruise/WOT; residual after pump-off | Gauge + tee |
| 7 | Coil part number, primary resistance, spark | DMM + scope |
| 8 | Sensor part numbers and transfer curves | Part numbers + voltage sweeps |
| 9 | Oil pressure sender type | Visual + DMM |
| 10 | Wheel-speed and gear sources | Visual |

Each result is entered into the GT650 engine profile with `source_ref` = the measurement
record ID and `provenance = BENCH-MEASURED`.

---

## 10. Vehicle and endurance

- Track session: torque coordination, TC, flat shift, thermal behaviour, pit limiter.
- Drag session: launch, shift strategy, elapsed-time logging, traction.
- Endurance: a multi-hour run with logging active, checking flash wear, memory, thermal
  drift, and drift in the calibration (the learner must not walk).

---

## 11. Adversarial review (§125, §125 phase)

An independent pass, before release, attacking:

crank-sync bugs · wrong TDC · wrong cylinder phase · injector resurrection · ignition
resurrection · excessive dwell · excessive injector duty · timer overflow · race
conditions · stale sensor data · DMA failure · CAN failure · calibration corruption ·
bootloader failure · watchdog starvation · brownout · plausibility bypass · unsafe
performance mode · launch/traction conflicts · quickshift conflicts · knock failure ·
overboost failure · thermal failure · logging-induced timing faults.

**Confirmed defects are fixed, not documented.** The review produces a findings list with
a status per item, and the release is blocked on any open CRITICAL/HIGH finding.

---

## 12. CI gates

| Gate | Blocks merge when |
|---|---|
| Build | any warning, any error |
| Unit tests | any failure |
| Coverage | a decrease on the portable core |
| Sanitizers | any ASan/UBSan report |
| Static analysis | any new clang-tidy/cppcheck finding |
| Layer check | a forbidden include or import |
| Schema codegen | the generated files differ from the committed ones |
| Default-vs-bounds | a shipped default violates its own bounds |
| Licence scan | a GPL or unknown-licensed dependency appears |
| Release build | a development bypass flag is enabled |

---

## 13. Status

**No test has been executed yet.** This is the plan that the implementation must satisfy.
Test IDs used in the other specifications (T1…T15, F1…F18, I1…I14, S1…S14, A1…A15,
TM1…TM12, C1…C12, PR1…PR10, L1…L10, D1…D12, A1…A12 autotune) are the authoritative
acceptance definitions; each will map to an implemented test with a name and a location
when its phase begins.
