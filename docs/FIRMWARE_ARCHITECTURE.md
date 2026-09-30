# FIRMWARE ARCHITECTURE

**Document ID:** FW-001 · **Status:** PHASE 1

---

## 1. Module graph

```
                        ┌──────────────┐
                        │   bootloader │  separate image, minimal, no engine logic
                        │  (security)  │  verifies signature, A/B slot, recovery
                        └──────┬───────┘
                               │ jump
   ┌───────────────────────────▼──────────────────────────────────────────────┐
   │                              APPLICATION                                 │
   │                                                                          │
   │  core/       types, units, ring buffers, crc, math, fixed-point,          │
   │              time base, event log, reset cause, fault codes              │
   │                                                                          │
   │  engine/     engine_configuration, engine_state, engine_geometry,        │
   │              volumetric_model, cylinder_assignment, engine_cycle         │
   │                                                                          │
   │  trigger/    trigger_core (state machine + engine angle),                │
   │              vr_input, hall_input, missing_tooth, multi_tooth,           │
   │              cam_sync, trigger_profiles, sync_manager, tooth_logger      │
   │                                                                          │
   │  scheduler/  event_queue, event, deadline, generation guard,              │
   │              timer_backend, virtual_backend (SIL)                       │
   │                                                                          │
   │  sensors/    analog, digital, frequency, sensor_core, transfer fns,       │
   │              steinhart_hart, plausibility, freshness, filtering          │
   │                                                                          │
   │  fuel/       load, ve, target_lambda, fuel_mass, injector_model,         │
   │              dead_time, battery_comp, transient, decel, staged, learner  │
   │                                                                          │
   │  ignition/   ignition_map, corrections, dwell, knock_window,             │
   │              per_cylinder, spark_command                                 │
   │                                                                          │
   │  lambda/     narrowband, wideband_external, wideband_native(CJ125),      │
   │              closed_loop, per_cylinder, heater                            │
   │                                                                          │
   │  knock/      knock_core, knock_filter, knock_window, knock_retard         │
   │                                                                          │
   │  boost/ vvt/ idle/   strategy modules, all capability-gated                │
   │                                                                          │
   │  torque/     torque_manager, arbitration, priority matrix, trace         │
   │                                                                          │
   │  launch/ quickshift/ traction/  performance features → torque requests   │
   │                                                                          │
   │  safety/     limits, safety_envelope, protection_actions, dtc,           │
   │              freeze_frame, watchdog supervisor                            │
   │                                                                          │
   │  can/        can_driver, can_db (signal mapping), tx scheduler            │
   │  communication/ protocol, framing, session, auth, calibration protocol,  │
   │              firmware_update, telemetry                                   │
   │  calibration/ schema mirror, transactions, CRC, versions, diff/merge      │
   │  logging/    data_logger, trigger_logger, event_logger, ring, storage     │
   │  security/   identity, keys, session auth, audit, debug lock              │
   │  diagnostics/ health, eol_test, self_test                                │
   │                                                                          │
   │  ─────────── target boundary (the ONLY code that knows the MCU) ────────  │
   │  hardware/   hal_adc, hal_dma, hal_timers, hal_gpio, hal_fdc, hal_usb,   │
   │              hal_flash, hal_storage, hal_wdt, hal_crypto, hw_profile      │
   └──────────────────────────────────────────────────────────────────────────┘
```

**Rule:** a module in `firmware/core` … `firmware/diagnostics` may not include any header
from `firmware/hardware` except through a declared interface. This is enforced by a
build-time include-guard script in CI (`tools/analysis/layer_check.py`, planned).

---

## 2. Execution model

Four static loops, no preemption, fixed order. Priority is expressed by ordering, not by
scheduler.

| Loop | Rate | WCET target | May be delayed by |
|---|---|---|---|
| **L1 CRANK ISR** | per CKP/CMP edge (up to ~25 kHz aggregate) | ≤ 6 µs (design target) | nothing — highest order, hardware only |
| **L2 CONTROL** | once per 720° engine cycle, or 2 kHz fallback, whichever is coarser-safe | ≤ 60 µs | L1 only |
| **L3 FAST** | 10 kHz fixed | ≤ 15 µs | L1, L2 |
| **L4 SLOW** | 200 Hz–1 kHz fixed (commits, storage, comms, logging) | budgeted | L1–L3 |

Nothing in L3/L4 may hold a lock, allocate, call into a blocking transport, or wait on a
peripheral used by L1/L2. `L1` and `L2` are *the* hard real-time region.

**Inter-task data exchange:** double-buffered `EngineState` with an atomic generation
counter. A reader copies the buffer, re-reads the generation, and retries if it changed.
No locks, no shared mutable state between loops.

**Deadline instrumentation:** every loop records `enter`, `exit`, and the maximum observed
`exit − deadline` into a health structure that the tuner reads continuously. This is how
§91's "measure it" requirement is satisfied in production, not just in the lab.

---

## 3. Hard real-time region: what is forbidden

```
FORBIDDEN in L1/L2 (enforced by code review + static analysis + a runtime guard):
  malloc / free / new / delete
  blocking waits, spin on peripheral flags
  flash erase/program
  SD/eMMC/SPI transfers
  USB, CAN transmit retries beyond a bounded count
  any logging to storage
  unbounded loops (every loop has a compile-time or calibration bound)
  recursion
  division by a non-constant unless the divisor is provably non-zero (documented)
  float printf / library calls
```

`L1` additionally forbids: map lookups with validation branches beyond a cached hot path,
any allocation, and any call that can take a hardware fault (a fault handler returns
without corrupting engine state).

---

## 4. Timing and scheduling model

### 4.1 Angle → time conversion

The position sensor produces **events at known angles**, not a continuous position
(`ENGINE_CONTROL_SPEC.md` §4). Scheduling is therefore expressed as an offset from a
specific event:

```
schedule(event E, offset_angles θ)  ⇒
    t = θ / (360 · rpm / 60)      [seconds, at the *current measured* rate]
```

and, to survive acceleration, the rate is **predicted** at the target angle:

```
rpm_pred(angle) = rpm_now · (1 + α · Δt)          α = d(rpm)/dt from the phase tracker
```

If prediction is disabled (engineering setting), `rpm_now` is used and the error is
logged. Prediction is *bounded* (see `TRIGGER_SPEC.md` §7); an implausible acceleration
estimate disables prediction rather than extrapolating.

### 4.2 Event lifecycle

```
struct Event {
    EventType   type;        // INJ_ON, INJ_OFF, COIL_ON, COIL_OFF, VVT, AUX, ...
    uint8_t     cylinder;    // 0..3
    uint16_t    generation;  // bumped on every re-plan; stale events are dropped
    uint32_t    target_tick; // absolute 1 µs tick
    uint32_t    deadline;    // latest acceptable execution tick
    uint8_t     priority;    // 0 = highest
    EventState  state;       // PENDING, ARMED, FIRED, CANCELLED, EXPIRED
};
```

Invariants (each is a unit test):
1. **No resurrection.** An event that has been `FIRED` or `CANCELLED` with generation *g*
   can never fire again unless a *new* event with generation > *g* is created.
2. **No duplicates.** The queue rejects an identical (type, cylinder, generation).
3. **Stale rejection.** An event whose generation is older than the current plan
   generation is discarded, never executed.
4. **Expiry.** An event not executed by its `deadline` is marked `EXPIRED`, the expiry is
   counted, and — for injection/ignition — it triggers a defined safety action rather than
   a silent late pulse.
5. **Wrap safety.** All tick arithmetic is unsigned 32-bit and uses explicit wrap-safe
   difference functions; a "next tick" computed in the past is treated as "next cycle".
6. **Collision handling.** Two events for the same hardware channel in the same window are
   a configuration error, caught by the validator before the engine is armed, and at
   runtime by an assertion that drops the lower-priority event.

### 4.3 Scheduling resolution

With a 1 µs timebase at 12 000 rpm, 1 µs ≈ **0.2° of crank**. Spark timing resolution is
therefore bounded by the timebase, not by the CPU. The design target is:
`|commanded − delivered| ≤ 5 µs` on the bench (`UNK-FW-002`, `UNK-FW-003`) — to be
*measured*, with the measurement method in `HIL_SPEC.md`.

---

## 5. Interrupts

| IRQ | Priority class | Work |
|---|---|---|
| CKP/CMP capture | 0 (highest) | Read capture register, advance phase model, fire due events. O(1). |
| Injector/ignition fault | 0 | Record fault, latch-disable the channel. |
| TIM compare (actuation) | hardware | The edge is produced by the timer; the ISR only *observes* for diagnostics. |
| DMA half/full (ADC) | 1 | Publish a sensor frame. Bounded work. |
| FDCAN | 2 | Bounded frame handling; never blocks. |
| TIM 1 µs / periodic | 2 | Loop pacing. |
| USB | 3 | Never touches engine state. |
| Flash/storage | 3 | Never touches engine state. |

**Priority grouping** is deliberate: engine-timing interrupts cannot be delayed by
communications or storage, and communication interrupts cannot delay the control loop.

---

## 6. Memory map and budget

| Region | Size | Contents |
|---|---|---|
| ITCM | 128 KB | Hot ISR code (L1, L2) — zero-wait-state, deterministic |
| DTCM | 128 KB | Engine state, scheduler queue, hot tables — no cache, no wait states |
| AXI/SRAM | 432 KB | Calibration image, logging buffers, comms, stack |
| Backup | 4 KB | Reset-cause latch, "is the engine running" flag, last fault |
| Flash A | ≤ 1 MB | Application image |
| Flash B | ≤ 1 MB | **Slot B for A/B update** (image is ≤ ~450 KB; the rest is headroom) |
| Bootloader | separate | Signature verification, A/B select, recovery UART path |
| Internal storage | SDIO/eMMC | Log ring, trigger log, event log, freeze frames |

**Invariant:** the hot real-time region (L1/L2 code + state + working tables) is entirely
inside ITCM/DTCM, so the cache can never stall a real-time path. This is a deliberate
architectural commitment, verified by a linker-script check in CI.

---

## 7. Flash / update strategy

1. Host signs the application image (Ed25519 or ECDSA-P256; final choice in
   `SECURITY_SPEC` pending `UNK-SW-003`).
2. Bootloader verifies the signature of the image in the inactive slot.
3. Image is written to the inactive slot with per-block CRC.
4. A commit record (with a second CRC, in a *different* flash page) marks the new slot
   valid. **Power loss before the commit record ⇒ the old slot runs.** This is the standard
   A/B guarantee and is the answer to "never brick the ECU because of an interrupted
   update" (§59).
5. Bootloader can be updated only from CAN/USB with the same discipline, and only by
   engineering mode.
6. If neither slot is valid, the ECU enters a **recovery state**: engine control is not
   armed, injectors and ignition are hardware-off, and a minimal USB/CAN recovery service
   accepts a new image. The vehicle is safe; the ECU is recoverable.

---

## 8. Diagnostics and health

`firmware/safety` and `firmware/diagnostics` maintain, continuously:

```
sync quality, last sync loss, sync loss count
max ISR latency, max scheduling error (inj / ign), control-cycle WCET
CPU load per loop (cycle counter deltas)
injector duty headroom (used vs limit), dwell headroom
battery voltage min/max, 5 V reference tolerance
sensor health bitmask (one bit per channel, with reason codes)
CAN bus error counters, bus-off count
logging: bytes written, write latency max, media errors, wear estimate
flash: CRC errors, image slot in use, calibration CRC
```

All of this is retrievable over the protocol and rendered in the ECU health page (§83).
This is not decoration: it is the evidence base for `SAFETY_CASE.md` and for the
"ECU health" differentiator.

---

## 9. Fault handling and the fault-injection surface

`firmware/core/fault.h` defines a single injection API used **only** by builds compiled
with `RACCOON_FAULT_INJECTION` (never in a release build — enforced by a compile error if
the flag is enabled in a release configuration).

Injectable faults (§90): missing tooth, false tooth, cam loss, sensor freeze, ADC error,
DMA error, CAN failure, lambda failure, battery sag, overvoltage, watchdog trip, timer
wrap, memory corruption (guarded test block), invalid configuration, output conflict.

Each injection is *deterministic* (seeded PRNG, scripted) so a test can assert an exact
recovery sequence. This is what makes §90's "verify deterministic recovery" testable
rather than aspirational.

---

## 10. Portability and the hardware profile

`firmware/hardware/hw_profile.h` is a *data* structure filled from
`hardware_profiles/<name>.json` at build time (generated header) or from flash at runtime
(read-only, CRC-protected).

The portable core only ever sees:

```c
typedef struct {
    /* timing */
    uint32_t (*tick_now_us)(void);
    int      (*arm_event)(const raccoon_event_t*);   /* returns 0 or an error code */
    void     (*disarm_event)(uint16_t generation);
    /* capture */
    int      (*capture_enable)(int channel, void (*cb)(uint32_t tick));
    /* analog */
    int      (*adc_start)(uint32_t rate_hz, void (*cb)(const raccoon_adc_frame_t*));
    /* coms */
    int      (*can_send)(const raccoon_can_frame_t*);
    /* storage (low priority only) */
    int      (*storage_write)(uint32_t offset, const void* buf, uint32_t len);
} raccoon_hal_t;
```

Two implementations: the real MCU HAL, and a *virtual* HAL that synthesises inputs from a
plant model and captures outputs for measurement. The SIL, the simulator, the HIL, and the
unit tests all use the virtual one. **This is the single most important structural decision
for testability**: the tests run the shipping engine code, not a re-implementation.

---

## 11. Build and static analysis (§100, §101)

```
CMake presets: debug | release | sil | fault-injection | coverage
Compiler flags: -Wall -Wextra -Werror -Wconversion -Wsign-conversion
                -Wshadow -Wcast-qual -Wstrict-prototypes -Wdouble-promotion (where intended)
                -fno-strict-aliasing is NOT used; strict aliasing enforced
Analysis: clang-tidy (readability, bugprone, performance, concurrency)
          cppcheck (--enable=warning,style,performance,portability)
          host sanitizers: ASan + UBSan on the full unit suite
          cppcheck-suppressions file, justified inline
          MISRA-inspired review checklist applied to L1/L2 code
Zero unexplained warnings — enforced by CI (any warning fails the build).
```

Release artifacts: `.elf`, `.bin`, `.map`, `.sym`, `manifest.json` (git commit, compiler
version, build flags, calibration schema version, hardware profile, hashes), and a
signature. Deterministic builds are verified by building twice and comparing hashes.

---

## 12. Relationship to the phase plan

| Phase | Firmware deliverable | Exit evidence |
|---|---|---|
| 2 Engine core | `engine/`, `trigger/`, `scheduler/` portable + virtual HAL | Trigger unit tests, sync-loss/recovery tests, 1–4 cyl scheduling tests, measured WCET in SIL |
| 3 Inputs | `sensors/` + ADC/DMA HAL | Sensor fault matrix tests (stale/open/short/rate/DMA) |
| 4 Fuel | `fuel/`, `lambda/`, `learner` | Fuel model tests, min/max pulse, battery/pressure compensation, cut precedence |
| 5 Ignition | `ignition/`, `knock/` | Dwell/angle tests, max-dwell hardware interlock test |
| 6 Protection | `safety/` | Every hard limit with a test that proves it wins over every requester |
| 7 Performance | `torque/`, `launch/`, `quickshift/`, `traction/`, `boost/`, `vvt/`, `idle/` | Arbitration matrix tests, fuzz of simultaneous features |
| 8 Data | `logging/` | Sustained-rate logging test with measured throughput |
| 9 Protocol | `can/`, `communication/`, `security/` | Protocol conformance, auth, power-loss-during-update test |
| 12 Validation | all | HIL timing measurement, fault-injection determinism |

---

## 13. Design note (clean-room statement)

All firmware in this project is **independently authored**. Public documentation from
Speeduino, rusEFI, MegaSquirt, Haltech, Link, FuelTech, MaxxECU, MoTeC, Bosch, Analog
Devices and NXP was consulted for *architectural ideas and published device parameters*
only. No GPL or proprietary code was copied, translated, or mechanically derived. See
[`THIRD_PARTY_LICENSE_REGISTER.md`](THIRD_PARTY_LICENSE_REGISTER.md) and the per-module
`DESIGN-NOTE` headers required by the clean-room protocol in §4 of that document.
