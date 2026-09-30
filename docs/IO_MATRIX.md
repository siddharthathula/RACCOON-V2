# I/O MATRIX AND TIMER BUDGET

**Document ID:** IOM-001
**Status:** PHASE 1 — logical budget for the reference hardware profile `raccoon_v2_rev_a`
**Rule (spec §92):** *the engine core must not know PCB pin numbers.* Everything in this
document is a **logical resource**, mapped to pins by `hardware_profiles/`.

**Coverage rule:** every logical resource in this document must resolve to exactly one
physical resource in a hardware profile, and a profile must declare *capabilities*. A
configuration that requests a capability the profile lacks is refused at validation
time (spec §97, §98) — not silently ignored.

---

## 1. Summary budget (reference profile, max engine configuration)

Target: 4 cylinders, sequential injection with staged secondary, sequential ignition,
2 knock channels, 4 lambda channels, 2 CAN buses, internal logging.

| Category | Provided | Max needed by RACCOON V2 (1–4 cyl) | Headroom |
|---|---|---|---|
| Position-sensor capture channels | 4 (CKP1, CKP2, CMP1, CMP2) | 2 minimum, 4 for dual-cam | 0–2 |
| Frequency/speed capture | 2 (front wheel, rear wheel) | 2 | 0 |
| Digital inputs (level, debounced) | 16 | 14 | 2 |
| Analog inputs (0–5 V) | 16 | 15 | 1 |
| Temperature inputs (thermistor, dedicated) | 4 | 4 | 0 |
| Knock conditioning inputs | 2 | 2 | 0 |
| Wideband/lambda analog inputs | 4 | 4 | 0 |
| Injector outputs | 8 (4 primary + 4 secondary) | 8 | 0 |
| Ignition outputs | 8 | 8 | 0 |
| Low-side power outputs (solenoids/relays, current-limited) | 8 | 7 | 1 |
| H-bridge outputs (DBW / stepper) | 2 | 2 (stepper OR DBW) | 0 |
| High-side / protected outputs | 2 | 2 (pump, main relay) | 0 |
| PWM outputs | 6 | 6 | 0 |
| CAN transceivers | 2 | 2 | 0 |
| USB | 1 | 1 | 0 |

This is a deliberately **wide, balanced** profile: it is the smallest profile that can
present the *complete* RACCOON feature set without lying about capabilities, which is the
central requirement of §94.

---

## 2. Input matrix

| Logical ID | Signal | Type | Electrical requirement | Validation | Used by | Notes |
|---|---|---|---|---|---|---|
| `IN_CKP1` | Crank position 1 | Capture | VR conditioner output (3.3 V, open-drain→pull-up) **or** Hall/digital with configurable pull | Edge filter, period plausibility, min-period, overspeed | Trigger, RPM, scheduler | Primary timing reference. Loss = immediate fuel/ignition inhibit. |
| `IN_CKP2` | Crank position 2 | Capture | as above | as above | Dual-wheel decoders, quadrature/direction | Optional; used for crank-only engines with a second pickup. |
| `IN_CMP1` | Cam position 1 | Capture | Hall/digital, polarity configurable | Edge filter, phase validation vs CKP | Cylinder phase, sequential operation | Required for sequential. |
| `IN_CMP2` | Cam position 2 (exhaust cam) | Capture | as above | as above | Dual-cam phase, VVT correlation | For DOHC engines. |
| `IN_WHEEL_F` | Front wheel speed | Capture/frequency | Hall, 12 V or 5 V, pull-up selectable | Frequency range, timeout | Traction control, speed, TC confidence | |
| `IN_WHEEL_R` | Rear wheel speed | Capture/frequency | as above | as above | Traction control, slip, shift timing | |
| `AI_MAP` | Manifold pressure | Analog | 0–5 V, protected, 10 kΩ series, RC filter | Open/short, rate, range, cross-check vs TPS | Load (speed density) | |
| `AI_MAP_2` | Second MAP / baro | Analog | as above | as above | Baro, differential pressure, redundancy | |
| `AI_TPS1` | Throttle position 1 | Analog | 0–5 V or 0.5–4.5 V | Calibration, stuck, rate, dual-channel rationality | Load, TC, launch, DBW | |
| `AI_TPS2` | Throttle position 2 | Analog | as above | as above | DBW redundancy, rationality | |
| `AI_PEDAL1` | Pedal position 1 | Analog | as above | as above | DBW request | |
| `AI_PEDAL2` | Pedal position 2 | Analog | as above | as above | DBW redundancy | |
| `AI_MAF` | Mass air flow | Analog/frequency | 0–5 V or frequency | Range, rate | MAF fuel strategy | Optional. |
| `AI_OILP` | Oil pressure | Analog | 0–5 V (ratiometric or absolute) | Range, rate, slow-filter, shutdown threshold | Engine protection | |
| `AI_FUELP` | Fuel pressure | Analog | 0–5 V | Range, rate, min-pressure | Dead-time compensation, protection | |
| `AI_EGT` | Exhaust gas temperature | Analog | Type-K/Type-J amplifier | Range, rate, slow | Protection, boost | |
| `AI_ETHANOL` | Fuel content | Analog/frequency | 0–5 V or 50–500 Hz | Range, rate | Flex fuel | Optional. |
| `AI_O2_1..4` | Wideband lambda 1–4 | Analog | CJ125/LSU 4.9 analogue, or controller CAN/serial | Range, heater status, controller status, switching rate | Closed loop, per-cylinder trim, autotune | Only valid with the controller present. |
| `AI_AUX_1..3` | User analog 1–3 | Analog | 0–5 V, protected | Generic | User functions | |
| `AI_VBAT` | Battery voltage | Analog | divided, protected | Range, rate, brownout | Dwell correction, protection, diagnostics | |
| `AI_5V_REF` | 5 V sensor reference | Analog (monitor) | measured, not driven | Tolerance check | Sensor plausibility (a drifted reference silently scales every 5 V sensor) | **Non-obvious but essential.** |
| `T_IN_ECT` | Engine temperature | Thermistor | dedicated pull-up, protected | Steinhart–Hart/table, open/short, rate | Cold start, ignition/fuel corrections, protection | |
| `T_IN_IAT` | Intake air temperature | Thermistor | as above | as above | Load correction, protection | |
| `T_IN_OILT` | Oil temperature | Thermistor | as above | as above | Protection, enrichment cut-back | |
| `T_IN_EGT_T` | Exhaust/EGT (thermocouple amp) | Analog in | see `AI_EGT` | as above | Protection | |
| `DI_CLUTCH` | Clutch lever switch | Digital | pull-up/pull-down, debounce 20 ms | State change rate, timeout | Flat shift, launch, auto-blip | |
| `DI_SHIFT_SW` | Shift paddle / switch | Digital | as above | as above | Flat shift, upshift | |
| `DI_LAUNCH` | Launch button | Digital | as above | as above | Launch control | |
| `DI_BRAKE` | Brake pedal/lever | Digital | as above | as above | Traction reference, pit limiter, rev-match inhibit | |
| `DI_PIT` | Pit limiter switch | Digital | as above | as above | Pit limiter | |
| `DI_KILL` | Kill switch | Digital | as above, fail-safe | as above | Emergency shutdown | **Fail-safe: engine control must not arm while active.** |
| `DI_SIDE_STAND` | Side stand | Digital | as above | as above | Vehicle safety (may remain with OEM) | |
| `DI_FLEX_SW` | Pressure switch | Digital | as above | as above | Fuel pressure interlock | |
| `DI_USER_1..6` | User digital 1–6 | Digital | configurable pull, debounce, timeout | as above | User functions | |
| `KNOCK_1..2` | Knock sensor 1–2 | Analog (conditioned) | band-pass 5–20 kHz, gain, envelope | Noise floor, signal presence, event rate | Knock control, per-cylinder retard | No sensor fitted ⇒ capability off. |

### Input requirements that are *not* optional
1. **`AI_5V_REF` monitoring.** If the 5 V rail collapses, every 5 V sensor reads
   incorrectly and plausibility checks can be fooled. Monitoring the reference is the only
   way to detect that class of failure. (Spec §16, §105.)
2. **Independent sensor-freshness timestamps.** A frozen value must expire (spec §16).
3. **Open/short detection thresholds per channel**, because a shorted 5 V reference can
   destroy a channel; hardware protection is required, not just a firmware clamp.

---

## 3. Output matrix

| Logical ID | Actuator | Type | Ratings required | Protection | Default/fault state | Used by |
|---|---|---|---|---|---|---|
| `OUT_INJ_P1..P4` | Primary injectors 1–4 | Low-side current, peak-and-hold capable | ≥ 4 A peak / ≥ 2 A hold, or smart-driver | Current limit, thermal, open-load detect, max-duty, hardware disable | Off (fuel-safe) | Injection |
| `OUT_INJ_S1..S4` | Secondary injectors 1–4 | as above | as above | as above | Off | Staged injection |
| `OUT_IGN_1..4` | Ignition 1–4 (logic-level coil or IGBT gate) | Digital, per-cylinder | 5 V logic / gate drive; flyback clamp path | Dwell max timeout in hardware, spark-duration feedback, coil-current limit | Off (no spark) | Ignition |
| `OUT_IGN_5..8` | Ignition 5–8 (wasted-spark / secondary set) | Digital | as above | as above | Off | Wasted spark on 4-cyl |
| `OUT_FUELPUMP` | Fuel pump | Low-side PWM/high-side relay | 3 A PWM or relay | Short circuit → latch off, retry policy | Off (engine stops safely) | Pump control |
| `OUT_MAINRELAY` | Main relay | Low-side | 300 mA | Over-current | Off | Power management |
| `OUT_IGN_IG` | Ignition enable / kill | Low-side | 500 mA | — | Off | Global ignition inhibit |
| `OUT_BOOST` | Boost solenoid | Low-side PWM | 1 A | Short/over-temp | Off | Boost control |
| `OUT_VVT` | VVT solenoid (×2) | Low-side PWM | 1.5 A | Current limit, phase feedback | Off | VVT |
| `OUT_IAC_PWM` | Idle air control (PWM) | Low-side PWM | 1 A | — | Off | Idle |
| `OUT_STEPPER_A/B` | IAC stepper coils | H-bridge | 500 mA | — | Off | Idle (stepper strategy) |
| `OUT_TACH` | Tachometer signal | Digital 0–5 V | 5 mA | — | Off | Dash |
| `OUT_SHIFT_LIGHT` | Shift light | Digital/PWM | 100 mA | — | Off | Dash |
| `OUT_WARN` | Warning lamp | Digital | 100 mA | — | Off | Dash |
| `OUT_PURGE` | Purge valve | Low-side PWM | 1 A | — | Off | Emissions-adjacent |
| `OUT_AUX_1..4` | User outputs | Digital/PWM | 500 mA each | Timeout, current limit | Off | User |
| `OUT_LAMBDA_HEAT` | Wideband heater enable (per channel) | Low-side | 1 A | Over-temp | Off | Lambda |

### Output requirements that are not optional
1. **Every output has a hardware-disable path** (a fault input that clears the output in
   hardware without CPU involvement). Spec §103/§105 demand deterministic failure, and a
   CPU that is hung cannot enforce a software default state.
2. **Ignition dwell maximum is enforced in hardware** (timer or smart driver), not only in
   software. An unbounded dwell is an explicit prohibition (§33, §103).
3. **Injector max duty / pulse overlap is enforced in hardware.**
4. **Every output declares a default state that is safe with the ECU unpowered or wedged.**
   For injectors and ignition that is unambiguously "off".

---

## 4. Timer and channel budget (the critical resource)

This is the resource that a naive design runs out of. Allocation is against the primary
MCU (STM32H7-class). All assignments are **logical**; the hardware profile maps them to
timer instances/channels.

### 4.1 Hard real-time timing

| Logical timebase | Resource | Resolution | Purpose |
|---|---|---|---|
| `TB_1US` | 32-bit basic/general timer in up-counting free-run | 1 µs | Global scheduler clock, all timeouts, logging ticks |
| `TB_TRIG` | 32-bit timer in **input-capture** mode on `IN_CKP1` | 1 µs | Tooth period measurement, acceleration, scheduling anchor |
| `TB_TRIG2` | 32-bit timer capture on `IN_CMP1` | 1 µs | Cam edge capture, phase measurement |
| `TB_ADC` | 32-bit timer triggering ADC (TRGO) | 1 µs | Phase-aligned sensor sampling |

### 4.2 Actuation channels (hardware edge generation — never software)

| Output group | Count | Timer requirement |
|---|---|---|
| Primary injectors | 4 | 4 independent compare channels, PWM mode for peak-and-hold |
| Secondary injectors | 4 | 4 independent compare channels |
| Ignition charge-start | 4–8 | 4–8 compare channels, **with hardware max-dwell interlock** |
| Ignition spark-stop | 4–8 | same channels (complementary/second compare) or paired timer |
| VVT / boost / IAC / pump PWM | 6 | 6 PWM channels |
| Stepper / DBW H-bridge | 2 | 2 × dual-channel (4 compares) |
| Taux / tach / shift light | 4 | 4 GPIO or 4 compares |

**Minimum distinct compare channels required: 30 (8 inj + 8 ignition-charge + 4 ignition-spark
+ 6 PWM + 4 aux).** H7-class parts provide well over this via 4 advanced 32-bit timers
(4 ch each = 16) + 2 × 32-bit general (4 ch each) + 12 × 16-bit TIM. **Feasible with
headroom** — and this is precisely why MCU selection was done on timer architecture
rather than clock speed.

### 4.3 Budget risk (must be checked before schematic freeze)
`UNK-HW-002` — a formal pin/timer/channel **conflict linter** is required: it takes a
hardware profile plus a requested configuration and proves that (a) every requested
capability has a physical resource, (b) no two resources share a pin, (c) no two critical
timing functions share a timer instance, and (d) DMA channels are sufficient for the
ADC/CAN/logging streams. A schematic cannot be started before this tool exists.

---

## 5. DMA budget

| Stream | Direction | Trigger | Notes |
|---|---|---|---|
| ADC1 (12 ch) | Peripherals → memory, circular | `TB_ADC` | Dual-buffer; half-transfer ISR at 1/2 period |
| ADC2 (redundant/dual-simultaneous) | Peripherals → memory, circular | `TB_ADC` (in phase) | Must be phase-aligned with ADC1 |
| Injector driver diagnostics (if smart driver) | Peripherals → memory, burst | On error flag | Interrupt-driven, low rate |
| FDCAN1 RX/TX FIFO | Peripherals ↔ memory | CAN | Dedicated CAN DMA/queue |
| FDCAN2 | as above | CAN | |
| Logging write | CPU → memory | CPU | Internal SDIO/eMMC via dedicated peripheral; never shares the ADC path |
| USB (if CDC) | Peripherals ↔ memory | USB | |

**Rule:** the ADC DMA streams must have a **dedicated, uncontended** path. Logging and USB
must not be able to delay an ADC half-transfer or a timer ISR. This is enforced by
priority assignment and by measurement (`UNK-FW-004`).

---

## 6. Capability model (what the profile exposes to the GUI)

The profile declares, and the firmware reports in the capability frame:

```
CAP_CKP              CAP_CMP              CAP_SEQUENTIAL        CAP_SEMI_SEQUENTIAL
CAP_BATCH            CAP_WASTED_SPARK      CAP_STAGED            CAP_WIDEBAND
CAP_NARROWBAND       CAP_MULTIPLE_LAMBDA   CAP_KNOCK             CAP_VVT
CAP_BOOST            CAP_TRACTION         CAP_DBW               CAP_FLEX_FUEL
CAP_CAN_FD           CAP_STEPPER_IAC       CAP_ANTI_LAG          CAP_INTERNAL_LOG
CAP_SPARK_FEEDBACK   CAP_INJECTOR_DIAG     CAP_GEAR_SENSOR       CAP_CLUTCH_SENSOR
```

Rules:
- A capability is **on** only if the physical resource exists *and* passes end-of-line test
  (spec §99) recorded against the unit's serial number.
- The GUI **adapts**: workspaces for unavailable features are hidden or shown as
  `NOT AVAILABLE — hardware profile lacks CAP_x`, never as broken controls (spec §94).
- A requested feature that exceeds the profile is refused by the configuration validator
  with a specific reason (§97, §98).

---

## 7. Resource decision summary

| Resource | Reference profile | Rationale |
|---|---|---|
| Position capture | 2 crank + 2 cam | dual-cam engines; second crank pickup for crank-only decoders |
| Injectors | 8 | 4-cyl sequential + staged |
| Ignition | 8 | 4-cyl sequential or wasted spark pairs, with 4 spare for secondary coil sets |
| Analog | 16 + 4 temp | matches the richest 1–4-cyl configuration (boost + flex fuel + EGT + 2 MAP + 4 O2) |
| Knock | 2 | dual-sensor knock with cylinder association |
| CAN | 2 | vehicle + expansion/dash |
| Logging | internal, target 512 MB–8 GB | Link's uniform-logging lesson; capacity set by measurement (`UNK-HW-009`) |
| Digital | 16 | clutch, brake, launch, shift, pit, kill, stand, pressure switch + 6 user |
