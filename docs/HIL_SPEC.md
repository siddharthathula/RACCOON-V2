# HIL SPECIFICATION

**Document ID:** HIL-001 · **Status:** PHASE 1

Purpose: prove, by measurement, the things that cannot be proven by reading code (§91) —
scheduling error, ISR latency, ADC latency, CAN latency, and the behaviour of a real ECU
under real electrical conditions and real faults.

---

## 1. Bench architecture

```
 ┌──────────────────────────────────────────────────────────────────────┐
 │  SIGNAL GENERATION                    ECU UNDER TEST                  │
 │  ┌────────────────┐                   ┌──────────────┐               │
 │  │ crank/cam      │  ── 12 V/5 V ──▶  │  CKP/CMP     │               │
 │  │ simulator      │                   │              │               │
 │  │ (FPGA or        │                   │  sensors     │  ┌──────────┐ │
 │  │  PC + line drv) │  ── 0-5 V ──────▶ │  analog      │  │ power    │ │
 │  │                 │                   │              │  │ stage    │ │
 │  │ TPS/MAP/Temp    │  ── 0-5 V ──────▶ │  analog      │  │ inj/ign  │ │
 │  │ generators      │                   │              │  │ aux      │ │
 │  │                 │  ── logic ──────▶ │  digital     │  └────┬─────┘ │
 │  │                 │                   │              │       │       │
 │  │                 │        ┌──────────┴──────────────┐       │       │
 │  │ CAN messages ◀──────────┤      STM32              │       │       │
 │  │                 │        │  (real firmware)       │       │       │
 │  └────────────────┘        │                        │◀──────┘       │
 │                            │  USB ──▶ host (tuner)  │  measure      │
 │                            └────────────────────────┘                │
 │                              probes: current (injector),             │
 │                              primary/secondary (ignition),            │
 │                              timing (logic analyser / scope)          │
 └──────────────────────────────────────────────────────────────────────┘
```

**Non-negotiable:** the ECU under test runs the **shipping firmware image**, connected
through the **real hardware profile**, and the host uses the **real protocol**. A
reimplementation in the test rig is not a HIL test.

---

## 2. Generated signals

| Signal | Requirement |
|---|---|
| CKP | 1 Hz to the design overspeed; any tooth count; noise injection; missing-tooth and false-tooth injection; acceleration and deceleration profiles; deliberate stall and restart |
| CMP | configurable teeth/phase, cam-loss injection, polarity inversion |
| TPS | 0–5 V and 0.5–4.5 V, slow and fast sweeps, stuck, open, short |
| MAP | 0–4 bar (or the profile's range), vacuum and boost, fast step |
| IAT / ECT | resistor-network simulation of the actual sensor, or direct voltage; open/short |
| Lambda | narrowband switching signal; wideband controller simulation (or a real controller on the bench) |
| Wheel speed | 0–200 Hz per channel, with slip scenarios |
| Battery | 6 V (cranking sag) to 18 V (alternator overvoltage), with transient steps |
| CAN | valid traffic, flood, malformed frames, bus-off injection |
| Switches | clutch, brake, launch, shift, pit, kill, stand, user |

The crank/cam generator must be able to run **faster than real time** for endurance runs,
and must produce **edge-accurate** timing so that a scheduling error measurement is
trustworthy (generator jitter must be an order of magnitude below the 5 µs target).

---

## 3. Measured quantities and how

| Quantity | Method | Target |
|---|---|---|
| Ignition scheduling error | Logic analyser: commanded compare (a debug GPIO toggled at the compare instant) vs. the actual primary-current edge | ≤ 5 µs, worst case, across RPM (closes `UNK-FW-002`) |
| Injector scheduling error | Current probe on the injector; commanded compare GPIO as the reference | ≤ 5 µs (`UNK-FW-003`) |
| Max ISR latency | A GPIO is set at the interrupt entry and cleared at the highest-priority return, captured by a logic analyser; run with all subsystems active | measured max, published |
| Minimum crank-to-scheduler latency | GPIO set at the CKP capture ISR entry, cleared when the next event is armed | measured min/max |
| ADC latency | ADC trigger → DMA complete timestamp, instrumented in firmware and cross-checked with a scope | measured |
| CAN latency | Host sends a timestamped frame; the ECU echoes it; host measures round trip, at 3 bus loads | measured distribution |
| Logging throughput | Sustained rate-group configuration; measure sustained frames/s and the maximum drop rate | measured, published |
| CPU load | Cycle-counter deltas per loop, published per loop | measured |
| RAM / flash | Linker map + runtime high-water mark | measured |
| Dwell accuracy | Current probe on the coil primary | measured against the commanded dwell |
| Max dwell enforcement | Command a dwell beyond the limit | hardware terminates; verified with the CPU halted |
| Brownout / reset | Power supply sweep | deterministic state, reset cause logged |

**Every number in the release notes comes from this table, with the bench configuration
recorded.** No number is quoted from a datasheet or from code inspection.

---

## 4. Fault injection (§90)

| Injected fault | Method | Expected verified behaviour |
|---|---|---|
| Missing crank tooth | generator drops a pulse | detected, phase maintained, event recorded |
| False crank tooth | generator inserts a pulse | rejected, counted, logged |
| Cam loss | generator stops CMP | sync policy engages; sequential operation inhibited |
| Sensor freeze | generator holds a value | `FROZEN` within the window; fallback; DTC |
| ADC error | firmware fault-injection build | `ADC_FAULT`; invalid; no stale value used |
| DMA error | firmware fault-injection build | `DMA_FAULT`; invalid; safe fallback |
| CAN failure / bus-off | CAN analyser | detected; engine unaffected; recovery per policy |
| Lambda failure | disconnect / invalid signal | closed loop exits; fallback; DTC |
| Battery sag | programmable supply | injector widening, ignition policy, brownout behaviour |
| Overvoltage | programmable supply | protection, deterministic state |
| Watchdog trip | firmware fault-injection build | reset with the correct cause; outputs off |
| Timer wrap | long-run or forced wrap | behaviour identical to a short run |
| Memory corruption | guarded test block with a flipped value | detected by ECC/CRC; safe configuration; never runs on corrupt data |
| Invalid configuration | inject a bad image | refused at validation; engine not armed |
| Output conflict | attempt a conflicting assignment | refused at validation |
| CPU halt | freeze the core (debugger or a test hook) | hardware protection still de-energises coils and injectors |

**The last row is the most important.** It proves §33/§103 claims in a way no amount of
software review can.

---

## 5. Test procedure

1. **Pre-test:** record board revision, firmware build hash, hardware profile, bench
   configuration, ambient temperature, and supply settings into the report header.
2. **Warm-up:** run a standard 20-minute profile so thermal results are comparable.
3. **Timing sweep:** the RPM sweep with edge-accurate signals; capture and analyse.
4. **Fault matrix:** every injection with a scripted expectation.
5. **Power events:** the full battery matrix including cold-crank sag.
6. **Endurance:** 8 h at the maximum logging rate with CAN and USB active.
7. **Report:** a machine-readable JSON + a human summary, with every measurement's method,
   conditions, and pass/fail.

---

## 6. Acceptance criteria

| Criterion | Requirement |
|---|---|
| Timing | ignition and injector scheduling error ≤ 5 µs worst case, measured |
| Determinism | identical commanded/measured timing with USB, CAN and logging at maximum activity |
| Protection | every hardware protection survives a halted CPU |
| Recovery | every injected fault has a defined, verified, deterministic recovery |
| No interference | no injected fault perturbs the control loop beyond the published bound |
| Reproducibility | the same bench configuration reproduces the same numbers within a stated tolerance |

---

## 7. Status

**This document is a plan.** No HIL bench exists yet, and therefore no timing claim is
made anywhere in this repository. The first HIL results will be recorded here with dates
and configurations when Phase 12 begins.
