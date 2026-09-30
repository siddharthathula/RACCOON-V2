# SAFETY CASE

**Document ID:** SC-001 · **Status:** PHASE 1 — the *structure* of the argument. No safety
claim is verified yet, because no hardware or test results exist. This document states
what must be *proved*, by what means, and what evidence will satisfy it.

---

## 1. Safety goal (top-level)

**Goal:** RACCOON V2 shall not cause engine damage or an unsafe vehicle state under any
combination of: user misconfiguration, sensor failure, electrical disturbance, signal loss,
interrupted update, thermal overload, and simultaneous feature demands.

This is an **aftermarket motorsport** product. It is **not** ASIL-rated and makes no
functional-safety certification claim. The argument below is a *engineering* safety case:
hazard identification, control measures, and verification evidence.

---

## 2. Hazards and controls

| ID | Hazard | Control | Verification |
|---|---|---|---|
| H-01 | Fuel injected into a cylinder on its exhaust stroke (wrong phase / firing order) | Cam/gap phase required for sequential; validation refuses; arming gate requires a confirmed TDC; non-`SYNCED` states inhibit fuel | Arming-gate tests; trigger state tests; **engine test with the timing light** |
| H-02 | Spark at the wrong angle (advanced beyond safe) | Hard ignition envelope applied last, non-bypassable; corrections applied before the clamp | Property test over the correction space |
| H-03 | Unbounded dwell → coil/IGBT destruction | Hardware max-dwell timer independent of the CPU; software clamp; coil-on timeout watchdog | HIL with the **CPU halted** |
| H-04 | Injector stuck open / excessive duty | Hardware duty ceiling; current limit; stuck-on detection; fuel cut on fault | Actuator tests A3, A4 |
| H-05 | Over-fuelling → lean-after-shutdown backfire, flooding | Max pulse, max duty, enrichment bounds, cut precedence | Fuel tests |
| H-06 | Knock damage | Knock control, escalation, persistent-knock shutdown; no claim without a sensor | Ignition tests I8, I9 |
| H-07 | Over-heat damage | ECT/oil/EGT limits with RETARD → LIMIT → CUT escalation | Engine test |
| H-08 | Oil starvation damage | Oil pressure limit with latching cut and recovery hysteresis | Engine test |
| H-09 | Over-rev (valve float / overspeed) | Hard RPM limit with cut, hysteresis, and hardware-inhibit path on overspeed | HIL + engine test |
| H-10 | Loss of sync at speed | Immediate fuel+spark inhibit, resynchronisation policy, per-mode | Trigger tests T9–T12 |
| H-11 | Stale/frozen sensor used as valid | Freshness + sequence + range + plausibility on every sensor; fallbacks; no silent staleness | Sensor tests |
| H-12 | Calibration corruption → nonsense commands | A/B slots, per-block CRC, signature, transactions, verify-after-commit, never run on corrupt data | Calibration tests C4, C5 |
| H-13 | Bricked ECU by an interrupted update | A/B + commit record; recovery mode with engine control disarmed | PR6 |
| H-14 | Brownout/battery disconnect causing an indeterminate state | Deterministic default states, hardware defaults, reset cause latching, power tests | Power matrix |
| H-15 | Launch/TC/shift conflict causing a control loss | Torque-manager arbitration, fixed priority, latches owned by one subsystem, fuzz test | TM1–TM12 |
| H-16 | Logging/CAN/USB causing a timing fault | Hardware-generated edges, priority isolation, HIL stall test, measured latency | HIL |
| H-17 | Unauthorised write to safety limits | Roles, authenticated sessions, signed audit log, no universal password | Security review |
| H-18 | Unsafe configuration accepted | Validator + wizard + arming gate with specific reasons | C11 |
| H-19 | Overboost (when fitted) | Hard overboost limit → cut, non-bypassable | Feature test |
| H-20 | Runaway throttle (DBW, when fitted) | Dual-sensor plausibility, runaway detection, limp mode | Actuator test A13 |
| H-21 | Wrong output driven by the wrong subsystem | Per-channel ownership + configuration conflict validation | C15, A15 |
| H-22 | Timer wrap causing a missed or spurious event | Wrap-safe arithmetic, assertions, expiry with a defined action | Wrap tests |
| H-23 | Memory corruption altering a control value | ECC where available, CRC on calibration, bounds checks on all inputs, memory test in EOL | EOL memory test |
| H-24 | Reverse polarity / miswiring | Hardware protection; deterministic behaviour | ISO 7637-2, reverse-polarity test |

---

## 3. Argument structure (to be completed with evidence)

For each hazard, the release must contain:

```
Hazard → Control (design) → Verification (test id + level) → Evidence (report) → Status
```

Status is one of: `VERIFIED` · `PARTIALLY VERIFIED` · `MEASURED` · `HIL VERIFIED` ·
`ENGINE VERIFIED` · `DYNO VERIFIED` · `UNKNOWN` · `BLOCKED` (with the blocker named).

**A hazard with no evidence is `UNKNOWN` or `BLOCKED` — never "assumed safe".**

---

## 4. Failure-response philosophy

| Class | Response | Rationale |
|---|---|---|
| **Fail-operational** | Continue with reduced authority, logged | Loss of a comfort function must not stop a motorcycle mid-corner |
| **Fail-safe (controlled)** | Degrade to a defined conservative state (base timing, reduced torque, no boost) and continue | Safer than an abrupt stop for the rider, and recoverable |
| **Fail-safe (stop)** | Cut fuel and spark | Sync loss, hard protection limits, unresolvable sensor loss at speed |
| **Limp (recovery)** | Substitute a defined value, raise a DTC, request service | A single failed sensor should not end the ride |

**The choice per hazard is documented, not accidental.** Where a hazard can plausibly be
either "degrade" or "stop", the default is the one that does not require a human decision
within milliseconds.

---

## 5. Independence of safety from features

A structural argument (stronger than a test list):

1. The safety envelope is applied **after** the torque manager, on the final command.
2. It is not an input to any feature, so a feature cannot weaken it.
3. Race modes select calibration overlays, not code paths; hard limits live in the base
   image and are therefore global.
4. Engineering mode may only **lower** hard limits, and only with authentication + audit.
5. Hardware protections (max dwell, duty ceiling, overspeed inhibit) are independent of the
   CPU and therefore survive a firmware fault.
6. Fuel and spark have hardware default states that are safe with the CPU halted or the
   ECU unpowered.

---

## 6. Known gaps in the current safety case

| Gap | Status | Resolution path |
|---|---|---|
| No hardware exists | `BLOCKED` | Phase 12 |
| Trigger strategy for the GT650 is unknown | `BLOCKED` (GT profile) | `UNK-GT-001/002/003` + bench characterisation |
| Injector/coil characteristics unknown | `BLOCKED` (GT fuel/ignition calibration) | `UNK-GT-004/006` + flow/dwell measurement |
| Smart-driver vs discrete power stage undecided | `BLOCKED` (HW) | `UNK-HW-005` |
| ISO 7637-2 thresholds not set | `BLOCKED` (HW) | `UNK-HW-007` |
| No measured timing data | `BLOCKED` | HIL |
| No functional-safety standard compliance | out of scope by declaration | — |

This table is the honest state of the safety case today. It is intentionally visible.
