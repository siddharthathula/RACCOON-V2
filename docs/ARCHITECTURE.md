# RACCOON V2 — SYSTEM ARCHITECTURE

**Document ID:** ARCH-001 · **Status:** PHASE 1 · **Date:** 2026-09-29

---

## 1. Design principles (derived from the spec, applied everywhere)

| # | Principle | Consequence in the design |
|---|---|---|
| P1 | **Deterministic engine control** (§104, §133) | Static, priority-ordered, allocation-free execution. No blocking, no dynamic memory, no dependence on comms/UI/logging. Engine control must not be able to fail because a USB transfer stalled. |
| P2 | **Actuate in hardware, decide in software** (§91) | The CPU computes *when*; the timer hardware produces the edge. Measured scheduling error, not assumed. |
| P3 | **Physics, not application, in the core** (§6, §93) | Engine geometry → volumetric model. The core contains no `if GT650`. GT650 is a data profile. |
| P4 | **Never claim what is not present** (§21, §34, §38, §94, §124) | A capability is enabled only by measured hardware. The GUI cannot show a control for a capability the board lacks. |
| P5 | **Hard limits always win** (§110) | The safety envelope is the last stage in the fuel/ignition chain and is non-bypassable, including in "performance" modes. |
| P6 | **Bounded by construction** (§102, §103) | No unbounded dwell, pulse, boost, RPM, loop, retry, or memory. Bounds are compile-time or configuration-validated, never runtime-unchecked. |
| P7 | **Every unknown is visible** (§108, §124) | Provenance tags (`SOURCE-DERIVED / CALCULATED / SIMULATED / HIL-VERIFIED / BENCH-MEASURED / ENGINE-TESTED / DYNO-MEASURED / VEHICLE-VALIDATED`) travel with every value. |
| P8 | **Power loss is a normal operating condition** (§105) | Every write is transactional; every actuator has a hardware default; reset reason is logged. |
| P9 | **The GUI never touches hardware** (§121) | Strict layering: transport → protocol → data model → engine model → calibration model → app state → UI. |
| P10 | **Reversible and safe UX** (§117, §118) | Every destructive action shows consequences; every calibration change is a reviewable, revertable transaction. |

---

## 2. System block diagram

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                          VEHICLE / ENGINE                                    │
│   CKP ─┐                                                                        │
│   CMP ─┤   VR/Hall sensors, TPS, MAP, ECT, IAT, λ (narrow + wideband),        │
│   wheel┤   oil P/T, fuel P, EGT, pedal, switches, flex-fuel                  │
│  speed─┘                                                                       │
│                                                                              │
│   INJ 1..4  ◄── fuel rail / injectors                                          │
│   IGN 1..4  ◄── coils (logic-level or dumb + IGBT stage)                      │
│   IAC/VVT/BOOST/PUMP/AUX ◄── solenoids, relays, IAC                            │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ analogue/digital/power — protected, filtered, conditioned
┌───────────────▼──────────────────────────────────────────────────────────────┐
│                          ECU — HARDWARE PROFILE                              │
│  ┌────────────┐ ┌──────────────┐ ┌────────────┐ ┌────────────┐ ┌───────────┐ │
│  │ PROTECTION │ │ CONDITIONING │ │  DRIVERS   │ │  POWER     │ │ STORAGE   │ │
│  │ ESD/surge  │ │ VR cond.     │ │ Injector   │ │ 12V→5V→3V3 │ │ internal  │ │
│  │ clamp/rev  │ │ Hall input   │ │ peak&hold  │ │ reverse    │ │ log (SD/  │ │
│  │ polarity   │ │ filter/hyst. │ │ IGBT/ign   │ │ pol. LDO/  │ │ eMMC)     │ │
│  │            │ │ comparator   │ │ flyback    │ │ DC-DC      │ │ A/B flash │ │
│  └─────┬──────┘ └──────┬───────┘ └─────┬──────┘ └─────┬──────┘ └─────┬─────┘ │
│        └───────────────┴───────────────┴──────────────┴──────────────┘       │
│                              MCU (STM32H7-class)                             │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ TIMER/DMA/ADC/CAN-FD peripherals (produce all edges, all sampling)     │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │  USB CDC / CAN / (optional wireless)
┌───────────────▼──────────────────────────────────────────────────────────────┐
│                        RACCOON TUNER (PyQt6, Python)                         │
│  transport → protocol → data model → engine model → calibration model →       │
│  application state → UI                                                       │
│  Workspaces: dashboard, live data, tuning, fuel, ignition, lambda, knock,     │
│  startup, idle, launch, quickshift, traction, engine braking, boost, VVT,     │
│  CAN, diagnostics, logging, dyno, drag, track, performance, map manager,      │
│  firmware, project, settings                                                   │
│  Analysis: autotune (confidence-aware), ignition analyser, dyno/drag/track,   │
│  log viewer + playback, AI advisory assistant                                 │
│  Offline: full editing, simulation, analysis with no hardware                 │
└──────────────────────────────────────────────────────────────────────────────┘

     Simulator ──┐                  HIL bench ──┐
     (same wire protocol)          (generate CKP/CMP/sensors, measure actuators)
                 └──────────────┬─────────────────┘
                                ▼
                     SIL / test automation / fault injection
```

---

## 3. Firmware architecture in one view

```
                    ┌───────────────────────────────────────────┐
  HIGH PRIORITY      │  CRANK EVENT (hardware ISR, O(1), no alloc)│
  (never blocked)   │   capture → period → decode → advance angle│
                    │   → fire due events → enqueue next cycle   │
                    └───────────────┬───────────────────────────┘
                                    │ publishes EngineState (double-buffered)
                    ┌───────────────▼───────────────────────────┐
                    │  CONTROL CYCLE (once per engine cycle)     │
                    │  load → fuel mass → injector model →      │
                    │  ignition → dwell → safety envelope →     │
                    │  torque manager → schedule events         │
                    └───────────────┬───────────────────────────┘
                                    │
        ┌───────────────────────────┼────────────────────────────┐
        ▼            ▼             ▼             ▼            ▼
    SENSOR      TORQUE         PERFORMANCE   LAMBDA/        DATA
    VALIDATION  MANAGER       FEATURES       CLOSED LOOP    SERVICES
   (analog,                              (launch, TC,    (telemetry,
    digital, freq)                        shift, EBT)    logging, events)
                                                    │
                    ┌───────────────────────────────▼───────────┐
  MEDIUM            │  COMMS TASK (CAN, USB, protocol, logging)  │
  (isolated)        │  MUST NOT delay anything above              │
                    └────────────────────────────────────────────┘
                    ┌───────────────────────────────────────────┐
  LOW               │  BACKGROUND: idle, learner, diagnostics,  │
                    │  storage, self-test, engineering console   │
                    └────────────────────────────────────────────┘
```

Full module graph, interface contracts and the timing budget are in
[`FIRMWARE_ARCHITECTURE.md`](FIRMWARE_ARCHITECTURE.md).

### 3.1 Why non-RTOS

The spec demands determinism, bounded execution and no unbounded loops. A statically
scheduled bare-metal design with a small, explicit scheduler gives a *provable* worst-case
execution time and trivially guarantees no priority inversion (there are no priorities to
invert — there is a fixed order). Adding an RTOS buys preemption and dynamic priorities,
which are *features for a multi-purpose system and liabilities for an engine controller*.
Decision: **no RTOS in V2** (`UNK-FW-006` records the decision and its audit trail).
The HAL, CMSIS and device headers are the only third-party code in the MCU image, and only
in `firmware/hardware/`.

---

## 4. The control pipeline (the spine of the whole product)

Every fuel and ignition value flows through exactly one ordered pipeline. This is what
makes the product debuggable, and what makes §110 ("calibration hierarchy") literally true
in code rather than in documentation.

```
BASE MAP                 e.g. VE table, ignition table        (calibration)
   ↓
SENSOR CORRECTIONS       IAT, ECT, BARO, BATTERY, ETHERANOL   (calibration)
   ↓
LOAD / STRATEGY          speed-density | alpha-N | MAF | hybrid
   ↓
TARGET LAMBDA            table + flex-fuel blend + gear + mode
   ↓
FUEL MASS                mg/stroke from mass–balance
   ↓
INJECTOR MODEL           flow, dead time, battery comp., pressure comp., min/max pulse
   ↓
TRANSIENT                TPS/MAP AE, wall-wetting model, tip-in/out, decay
   ↓
CLOSED LOOP              narrowband or wideband correction, per-cylinder trim
   ↓
PERFORMANCE              launch, quickshift, traction, EBT, antilag, drag strategies
   ↓
TORQUE MANAGER           resolves conflicts between all requesters → allowed torque
   ↓
HARD SAFETY ENVELOPE     RPM, lambda-lean, temperature, oil, fuel pressure, duty,
                         dwell, knock, sync loss, battery, boost
   ↓
FINAL COMMAND            ms/stroke per cylinder, ignition degrees per cylinder
```

**Invariant:** the safety envelope is the last thing applied, and it is applied to the
*command*, not to the request. A torque manager bug cannot bypass it; a performance
feature cannot bypass it; engineering mode cannot bypass it (engineering mode can only
*lower* hard limits, and only with an authenticated session + audit entry).

**Invariant 2 (fuel cut is final):** once the decel/fuel-cut decision is made in this
pipeline, no later stage may resurrect fuel for that event. Minimum-pulse and enrichment
logic operate *upstream* of the cut, and the cut is a hard gate at the output stage too
(`FUEL_SPEC` §7).

**Invariant 3 (one path, fully observed):** every enrichment term is a named, logged
quantity. There are no hidden multipliers. The telemetry frame carries a *correction
vector* (base, sensor, strategy, injector model, transient, closed-loop, trim, performance,
safety) so the tuner can see the arithmetic, not just the result.

---

## 5. The torque manager as the coordination layer

The spec (§40) makes the torque manager central. Architecture:

```
requesters            torque_manager                    outputs
─────────            ──────────────                    ───────
rider request   ──┐                                      
launch          ──┤   (a) collect requests               allowed_torque_scale
traction        ──┤   (b) resolve by strict priority      ignition_request
quickshift      ──┤   (c) produce ONE command per event   fuel_request
rev limiter     ──┤   (d) log every arbitration           arbitration_trace
knock           ──┤   (e) never raise a hard limit
protection      ──┤   (f) never exceed a hard limit
boost protection──┘
```

Arbitration rules (testable, per `TORQUE_CONTROL_SPEC.md`):
1. **Monotone de-authority within a cycle.** A request can only reduce authority; authority
   granted this cycle cannot be spent in a later cycle without re-arbitration.
2. **Fixed priority for conflicting reductions**: protection > sync loss > knock > lean >
   thermal > traction > shift > launch > rider. Ties resolved by a documented table, never
   by ordering of function calls.
3. **Latching vs non-latching**: sync loss, lean, and low oil pressure are *latching*; a
   retarding action is applied for a calibrated hold time and then decays predictably.
4. **Traceable**: every arbitration emits a bitmask + the winning requester, so a log can
   explain the engine's behaviour exactly.

---

## 6. Calibration architecture

```
                        ┌──────────────────────────┐
   tools/codegen  ─────▶│  SCHEMA (YAML + types)   │  single source of truth
                        │  id, name, type, unit,   │
                        │  min, max, default,      │
                        │  source, validation,     │
                        │  dependencies, safety    │
                        └───────────┬──────────────┘
                                    │ generated
                ┌───────────────────┼────────────────────┐
                ▼                   ▼                    ▼
        C struct + getters    Python dataclasses    GUI form/limits
        (firmware)            (tuner)               (tuner)
```

- **Addressing:** stable 16-bit `CAL_ID`s, never positional. Adding a parameter does not
  move any existing parameter. Page/offset framing is an implementation detail of the
  wire protocol and is negotiated at connect time.
- **Ownership classes:** `READ_ONLY` (derived), `ENGINEERING` (engineering mode only),
  `TUNER` (live-editable), `SAFETY` (hard limits — engineering mode may only lower),
  `MODE` (per race mode).
- **Provenance per value:** `SOURCE-DERIVED | CALCULATED | MEASURED | SIMULATED | …`
  plus an optional `source_ref` (datasheet page, test record ID, measurement date).
- **Validation:** a dependency graph. Writing a value triggers validation of dependents;
  an invalid combination is refused with a specific reason. No silent clamping of
  safety-critical values.
- **Transactions:** `READ → VALIDATE → STAGE → CRC → COMMIT → VERIFY → ACTIVATE`, with
  rollback to the previous image on any interruption (power loss, bus error, timeout).
  Two image slots + a commit marker ⇒ no partial configuration is ever observable.
- **Version control:** every tune is a versioned object (version, timestamp, ECU serial,
  firmware build, hardware profile, engine profile, user, description, checksum) with
  save/duplicate/compare/rollback/branch/merge/export. A map-level three-way merge is
  provided for `merge`.

---

## 7. Communication architecture

| Layer | Responsibility | Rule |
|---|---|---|
| Transport | Bytes over USB CDC / CAN / optional wireless | Reconnect-safe, back-pressure aware, never blocking for the firmware |
| Protocol | Framing, sequencing, CRC, capabilities, auth, firmware update, calibration transactions | Versioned, self-describing, checksummed, replay-safe |
| Data model | Typed frames: telemetry, trigger, log, DTC, capability, event | Engine-timed fields carry a crank/cycle timestamp, not a receive timestamp |
| Engine model | The tuner's *independent* model of the engine (1–4 cyl) | Enables offline simulation and analysis without hardware |
| Calibration model | Typed schema, transactions, versions, diff/merge | Same schema as firmware, generated from one source |
| Application state | Projects, connections, dashboards, sessions, undo stack | Single source of truth for the UI; UI reads state, sends intents |
| UI | Workspaces, widgets, map editor, log viewer, analysers | Never imports transport or hardware code directly |

**Timing discipline:** every logged value carries `(engine_time, cycle, crank_angle, seq)`.
"Engine time" is derived from the crank, so a log is meaningful even if the transport
stalls, and playback reconstructs engine-accurate state.

---

## 8. Hardware abstraction

```
firmware/core/        portable C++17, host-compilable, zero hardware dependencies
firmware/engine/      portable engine control
firmware/trigger/     portable trigger decoder (fed by a timestamp stream)
firmware/scheduler/   portable scheduler core; back end = timer OR virtual (SIL)
firmware/sensors/     portable sensor logic; back end = ADC frames or simulated
firmware/...          all control logic portable
────────────────────────────── target boundary ──────────────────────────────
firmware/hardware/    MCU HAL: ADC+DMA, timers, GPIO, FDCAN, USB, flash, storage, WDT
                       implements interfaces declared by the portable core
hardware_profiles/    JSON: ADC map, timer map, GPIO map, CAN, storage, driver types,
                       capability set — no code, pure data
```

The same core source builds for the target, the SIL simulator, and the unit tests. This is
what makes §88–§90 (unit/integration/SIL/fault-injection) achievable at all: the tests
exercise the *shipping* code, not a model of it.

---

## 9. Verification architecture

| Level | What runs | Where | What it proves |
|---|---|---|---|
| Unit | Every module, host build, sanitizers | CI | Logic correctness, bounds, no UB |
| Integration | Subsystem chains (trigger→scheduler→injector) | CI | Interface contracts and timing assumptions |
| SIL | Whole engine core vs a plant model, incl. fault injection | CI + desktop | Closed-loop stability, recovery determinism, scheduler behaviour at 4 cyl |
| HIL | Real board + signal generator + actuator measurement | Bench | Real scheduling error, real ADC/CAN/flash behaviour, real power loss |
| Bench | Board bring-up, EOL test (§99), power pulses | Bench | Manufacturing repeatability, protection behaviour |
| Engine | Motorcycle on a stand, then a dyno | Track/dyno | Real combustion behaviour, knock, thermal, protection |
| Vehicle | Drag strip + track session | Track | Feature claims only |
| Adversarial | Independent attack on sync, resurrection, dwell, overflow, races | CI + review | §125 defect hunting |

**Nothing claims real-time performance from source inspection** (§91). Every timing number
in the release notes is measured, with the method and conditions recorded.

---

## 10. Data flow for one control cycle (worked example)

```
CKP edge 17 µs after predicted → ISR:
  • read capture → tooth period (measured 1 234 µs)
  • decode → tooth index 34/36, angle = 340°
  • advance phase model; compute RPM = 48 650 rpm-equivalent… (mathematically,
    then filtered with a phase-locked estimator)
  • fire due events from the pending queue (coil off, injector off)
  • enqueue events for the coming window (injector on ×N, coil on)
  • update EngineState (published to the cycle task via a double buffer + generation)
  • return                                                     [≈ 1.8 µs, measured]

Control cycle (on the 720° event, or a fixed 2 kHz tick if a decoder has no 720° event):
  • validate sensor freshness → plausibility → fallbacks
  • compute load (MAP − baro), target lambda (table × fuel × gear × mode)
  • fuel mass → injector model (dead time, battery, pressure, min/max)
  • transient model, closed-loop correction, per-cylinder trim
  • ignition base + corrections + knock
  • torque manager arbitration → single command set
  • safety envelope clamp
  • hand events to the scheduler with generation numbers and deadlines
```

---

## 11. Cross-cutting mechanisms

| Mechanism | Where | Notes |
|---|---|---|
| Watchdog hierarchy | `firmware/safety` | WDT1 for the control domain, WDT2 for the comms/background domain. Independent windows so a runaway background task cannot reset a healthy control loop, and a stuck control loop *does* reset. |
| Brownout / reset cause | `firmware/core` | Reset reason latched into the event log; deterministic post-reset state. |
| Flash wear | `firmware/calibration`, `firmware/logging` | Log in a ring; calibration only written on explicit commit; wear budget in the ECU health page. |
| Security | `firmware/security` | Signed firmware (Ed25519-style verify in bootloader), authenticated calibration session, per-device identity, production debug lock. Dev bypass is a build flag, and a release build *fails* if the flag is set. |
| Time base | `firmware/core` | Engine time (from crank) is the primary clock for logs; RTC/host time is metadata only. |
| Units | everywhere | SI internally; display units are a UI concern; every calibration entry declares its unit. |

---

## 12. Architecture decision log (summary)

| # | Decision | Alternatives rejected | Rationale |
|---|---|---|---|
| AD-01 | Bare-metal, statically scheduled, no RTOS | FreeRTOS/Zephyr | Provable WCET, no priority inversion, smallest failure surface (§103, §104) |
| AD-02 | Timer hardware generates all actuator edges | Software GPIO toggling | Cannot meet ±5 µs spec deterministically (§91) |
| AD-03 | ID-addressed calibration schema | Positional byte pages (rusEFI/MegaSquirt style) | Firmware and GUI evolve independently; no coordinated page-size lock |
| AD-04 | Engine geometry drives a volumetric model, not a per-engine code path | Per-engine conditionals | Universal 1–4 cyl (§6); GT650 becomes data |
| AD-05 | CRC + signature, A/B image slots, explicit transactions | Single image, best-effort write | No brick, no partial config (§56, §59) |
| AD-06 | Native wideband = CJ125 + LSU 4.9 on a daughterboard, capability-gated | Claiming wideband without hardware | §21, §34, §124 honesty rule |
| AD-07 | Desktop PyQt6 application, no ECU screen in V2 | Built-in touchscreen (FuelTech style) | Spec §61 defines a desktop product; documented non-goal |
| AD-08 | Three capability classes: reported / physical / tested | Binary "supported" | Prevents claiming an untested or absent feature (§94, §99) |
| AD-09 | Engine-time logging with crank-relative timestamps | Host receive timestamps | Logs remain valid under transport stalls; enables playback |
| AD-10 | Adapter CAN for third-party wideband/dash by default; native optional | Native only | Lowers NRE and hardware risk; capability-gated upgrade path |

---

## 13. What this architecture does *not* claim

- It does not claim any performance figure (§114).
- It does not claim that any hardware exists yet (Phase 1 is a paper design).
- It does not claim GT650 trigger/sensor specifics (see `GT650_RESEARCH.md` and
  `UNKNOWN_REGISTER.md`).
- It does not claim ASIL compliance. This is an aftermarket motorsport product.
