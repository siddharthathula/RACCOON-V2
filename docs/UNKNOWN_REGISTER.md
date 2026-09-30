# UNKNOWN REGISTER

**Document ID:** UR-001
**Status:** LIVE — every unresolved engineering question lives here.
**Governing rule (spec §108):** *Unknown must never silently become a guessed value.*
Any constant in the firmware or tuner that corresponds to an open item below must be
marked in-code with a reference to its `UNK-*` ID, must carry a conservative default, and
must be listed in the ECU's self-reported uncertainty (capability/diagnostics frame) so the
GUI can display it.

Fields: `ID · QUESTION · WHY IT MATTERS · CURRENT EVIDENCE · CONFIDENCE · MEASUREMENT REQUIRED · TEST METHOD · BLOCKING?`

**Confidence scale:** `NONE` · `LOW` · `MEDIUM` · `HIGH` (HIGH = measured or primary-source published for the *exact* unit).

**Blocking** = Phase cannot start / cannot ship without it. **Phase** = earliest phase in which it must be closed.

---

## A. GT650 application (target vehicle)

| ID | Question | Why it matters | Current evidence | Conf. | Measurement required | Test method | Blocking? | Phase |
|---|---|---|---|---|---|---|---|---|
| UNK-GT-001 | Crank trigger wheel tooth count / missing-tooth layout / sensor type (VR or Hall) | Defines the trigger decoder, sync strategy, and whether the crank signal is even usable as-is | Public sources give no trigger details | NONE | Tooth count, wheel photos, raw CKP waveform | Visual + scope/signal-analyser capture at crank and 6000 rpm | **Yes** — trigger firmware | 2 / 12 |
| UNK-GT-002 | Cam sensor presence, type, tooth count, phase | Sequential injection/ignition requires valid cylinder phase | No public data | NONE | Cam wheel + phase at TDC | Scope + strobe + cam observation | **Yes** — sequential config | 2 / 12 |
| UNK-GT-003 | Firing order and cylinder numbering / TDC offset | Wrong value = fuel into an exhaust stroke | Parallel twin, 180° crank is conventional | MEDIUM | Direct observation | Strobe + piston position | **Yes** | 2 / 12 |
| UNK-GT-004 | Injector part number, impedance, flow (cc/s @ bar) | Dead time, min pulse, duty ceiling, driver topology | None | NONE | Part number, DMM coil resistance, flow curve | Bench flow test at 3 bar/4.5 bar, or datasheet | **Yes** — fuel model | 4 |
| UNK-GT-005 | Fuel pressure idle/cruise/WOT; pressure after pump-off | Dead-time compensation, low-pressure protection | None | NONE | Pressure readings at 3 states | Gauge + tee | **Yes** — protection | 4 |
| UNK-GT-006 | Ignition coil architecture, primary resistance, turns ratio, saturation dwell | Dwell model, driver topology, CDI compatibility | Public: digital spark ignition, Bosch UR5CC plug | LOW | Part number, primary R, secondary spark presence | DMM + scope of primary current | **Yes** — ignition | 5 |
| UNK-GT-007 | TPS count and electrical format | Load strategy, DBW option | None | NONE | Part number, voltage sweep across throttle | DMM/scope while actuating | Yes (TPS) | 3 |
| UNK-GT-008 | MAP sensor presence, part, range | Speed-density viability | None | NONE | Part number, transfer curve | Pressure + voltage sweep | Yes (load) | 3 |
| UNK-GT-009 | ECT/IAT sensor curve (Ω@25 °C, β) | Cold start, ignition correction | None | NONE | R at 25 °C and 100 °C, part number | DMM + controlled temp | Yes (temps) | 3 |
| UNK-GT-010 | Lambda: fitting count/location, sensor type, wideband socket | Closed loop + per-cylinder feasibility | None | NONE | Sensor marking, voltage behaviour at rich/lean | Scope of O2 signal | Yes (lambda) | 3 |
| UNK-GT-011 | Idle actuator type and presence | Idle strategy | None | NONE | Visual + part number | Bench | No | 7 |
| UNK-GT-012 | Fuel pump drive scheme (on/off/PWM) + pressure sensor | Pump duty limits, limp mode | None | NONE | Scope of pump command at idle/WOT | Bench | No | 7 |
| UNK-GT-013 | Vehicle CAN presence, bitrate, IDs | Dash, wheel speed, gear, diagnostics | None | NONE | Bus survey | CAN analyser with vehicle | No | 9 |
| UNK-GT-014 | Wheel-speed source (ABS sensors) | Traction control + speed | None | NONE | Presence, signal shape | Scope | No (blocks TC only) | 7 |
| UNK-GT-015 | Gear position source | Torque management, TC, launch, pit limiter | None | NONE | Presence | Visual | No (blocks those features) | 7 |
| UNK-GT-016 | Clutch / shift switch presence | Flat shift, launch, auto-blip | None | NONE | Presence, pull-up scheme | Visual + DMM | No | 7 |
| UNK-GT-017 | OEM OBD/KWP capability | Post-install validation, free-air sensor check | Manual mentions MIL only | LOW | Interface availability | Bidirectional diagnostic in a safe state | No | 12 |
| UNK-GT-018 | Oil pressure / oil temperature sender | Engine protection | Manual has a low-oil-pressure indicator | LOW | Type (switch vs analogue) | Visual + DMM | No | 6 |
| UNK-GT-019 | Connecting-rod length / rod ratio | Volumetric-efficiency and pumping-loss modelling accuracy | Not published | NONE | Measurement | Direct measurement | No | 4 |

---

## B. Hardware / component / board

| ID | Question | Why it matters | Current evidence | Conf. | Measurement required | Test method | Blocking? | Phase |
|---|---|---|---|---|---|---|---|---|
| UNK-HW-001 | Final MCU part number and package | Drives timer/ADC/CAN pin budget and PCB | Candidates analysed in `MCU_SELECTION.md` | MEDIUM | Design review decision | Architecture review sign-off | **Yes** — schematic | 1/2 |
| UNK-HW-002 | Timer channel budget: can all injector + ignition + aux channels be assigned without conflict? | Missing a channel means missing a cylinder | Derived budget in `IO_MATRIX.md` | MEDIUM | Conflict check on the real pin map | Pin-map lint tool + schematic review | **Yes** | 1 |
| UNK-HW-003 | VR conditioner design: MAX9926 (dual) vs discrete; open-drain pull-up to 3.3 V vs 5 V; edge polarity | Trigger reliability in the presence of starter noise | MAX9926 datasheet verified; board design not done | MEDIUM | Bench verification with real CKP sensors | Signal generator + real sensor on a spinning jig; measure pulse fidelity at 25 kHz | **Yes** — input path | 1/2 |
| UNK-HW-004 | CJ125 supply, pricing, second source | Native wideband capability | Public availability only | LOW | Supplier confirmation + eval part | Purchase eval unit, bring-up on a bench | No (blocks native wideband) | 1 |
| UNK-HW-005 | Smart driver IC (NXP MC33810/MC33813) vs discrete injector/ignition stages | Determines peak-and-hold, spark-duration feedback, diagnostics | Datasheets verified | LOW | Thermal and EMC bench results | Bench: 4 channels, full duty, 30 min thermal soak | **Yes** — power stage | 1 |
| UNK-HW-006 | Ignition primary current-sense strategy per coil | Dwell accuracy and coil-current protection | FBx divider 9:1 from MC33810 datasheet | MEDIUM | Current-sense accuracy across 6–16 V | Bench with a known coil | **Yes** | 1/5 |
| UNK-HW-007 | Power architecture: 12 V → 5 V → 3.3 V, reverse polarity, load dump, ISO 7637-2 pulses, brownout behaviour | Survival on a motorcycle charging system is a hard requirement | Standard practice; specific thresholds not yet set | MEDIUM | Pass/fail on pulses and dumps | ISO 7637-2 test bench; ECU must survive and reset deterministically | **Yes** | 1 |
| UNK-HW-008 | Sensor supply budget: 5 V total current, per-channel protection, short-circuit behaviour | Prevents a failed sensor taking down the ECU | rusEFI public board lists dual 5 V, 150 mA each, fully protected | LOW | Real current draw of the chosen sensor set | Bench | **Yes** | 1 |
| UNK-HW-009 | Internal logging media: eMMC/SD endurance, write strategy, wear levelling | Logging is a product requirement (§52) | Not selected | LOW | Endurance + throughput measurement | Sustained 1 kHz × 60 channel logging for 24 h | No (blocks logging capacity claim) | 8 |
| UNK-HW-010 | Enclosure, connector, sealing, vibration, thermal range in an engine bay | Compact, robust product | Not selected | LOW | Thermal soak + vibration | Chamber + shaker | No | 14 |
| UNK-HW-011 | PCB stack-up/length matching for CKP and cam inputs | Timing integrity on a noisy harness | Not started | NONE | Signal integrity on the built board | Scope at 25 kHz with realistic harness | No | 2 |

---

## C. Firmware / algorithms

| ID | Question | Why it matters | Current evidence | Conf. | Measurement required | Test method | Blocking? | Phase |
|---|---|---|---|---|---|---|---|---|
| UNK-FW-001 | Minimum crank edge rate and the input filter's effect at 12 000 rpm | Missed tooth = lost sync = potential engine damage | Derived from tooth count (unresolved) | NONE | Measured minimum reliable edge interval | Signal generator sweep to the max designed RPM | **Yes** | 2 |
| UNK-FW-002 | Ignition scheduling error bound (µs) at 12 000 rpm | 1 µs ≈ 0.24° at 12 000 rpm; error compounds into timing error | Not measured | NONE | Measured max error | HIL: command vs measured spark edge, worst case | **Yes** — ignition acceptance | 12 |
| UNK-FW-003 | Injector scheduling error bound (µs) | Fuel metering error | Not measured | NONE | Measured max error | HIL with a current probe | **Yes** | 12 |
| UNK-FW-004 | Maximum ISR latency and the resulting worst-case scheduling delay | Sets the safety envelope | Not measured | NONE | Measured | GPIO toggle + LA, worst case with all subsystems active | **Yes** | 12 |
| UNK-FW-005 | Max CPU load with full logging + CAN + UI tasks at max cylinder count | Headroom | Not measured | NONE | Measured | Cycle-count instrumentation, 4-cyl worst case | Yes (release gate) | 12 |
| UNK-FW-006 | Whether a **non-RTOS, statically scheduled** design meets all deadlines | Simplifies determinism proof enormously | Architectural decision | HIGH (decision) | Deadline audit | Static analysis + measured margins | **Yes** — architecture sign-off | 1/2 |
| UNK-FW-007 | Wall-wetting / fuel-film model form for a motorcycle injector | Transient calibration quality | Literature only | LOW | Measured film decay after a fuel pulse | Bench: pulse then measure mixture decay | No | 4 |
| UNK-FW-008 | Knock sensor mounting and resonant frequency on this block | Knock detection is impossible without it | No sensor known to be fitted | NONE | Resonance sweep | Accelerometer + swept sine on the block | No (blocks knock) | 5/7 |
| UNK-FW-009 | Per-cylinder lambda methodology for a parallel twin with one sensor | Spec forbids inferring per-cylinder AFR without valid methodology | Method literature | LOW | Bench demonstration of correlation with per-cylinder exhaust sampling | Two-sensor experiment | No | 4 |
| UNK-FW-010 | Closed-loop authority and instability boundary for wideband + transient | Oscillation risk | Literature | MEDIUM | Stability margin measured in SIL/HIL | Sweep controller gain, find oscillation | **Yes** — closed loop | 4 |
| UNK-FW-011 | Long-term (4D) learner convergence and drift behaviour in traffic | Avoid self-inflicted lean | Design only | MEDIUM | Convergence time and drift under 1000-cycle simulation | SIL endurance | No | 4 |
| UNK-FW-012 | Torque-manager conflict resolution order under simultaneous launch + TC + shift + knock | Interlock correctness | Design only | MEDIUM | Fuzz test of the priority matrix | Property/fuzz test in SIL | **Yes** — safety | 7 |

---

## D. Calibration / software / product

| ID | Question | Why it matters | Current evidence | Conf. | Measurement required | Test method | Blocking? | Phase |
|---|---|---|---|---|---|---|---|---|
| UNK-SW-001 | Does the tuner need backward compatibility with MS/TunerStudio `.ini` and `.msl` files? | Interop vs scope | Not decided | LOW | User requirement | Product decision | No | 10 |
| UNK-SW-002 | License and distribution terms for shipping a closed-source tuner | Commercial | Not decided | LOW | Legal review | Legal | **Yes** — release | 14 |
| UNK-SW-003 | Cryptographic scheme: per-device key provisioning, signature algorithm, root key custody | Spec §59/§60 | Design | MEDIUM | Implementable, reviewable design | Security review | **Yes** — production | 9 |
| UNK-SW-004 | Whether to ship an AI assistant at all in V2, and with what data boundary | Spec §84/§115 says optional/advisory | Undecided | MEDIUM | Product decision + privacy review | Product decision | No | 11 |
| UNK-SW-005 | Log format stability: must old logs open in new versions forever? | User trust | Design | MEDIUM | Format versioning plan | Design review | No | 8 |
| UNK-SW-006 | Whether `pyqtgraph` is sufficient for a 1 kHz × 60-channel log viewer at interactive rates | GUI responsiveness (§120) | pyqtgraph present locally | MEDIUM | Measured frame time | Benchmark on target hardware | **Yes** — GUI acceptance | 10 |
| UNK-SW-007 | Python version floor and package lock strategy for reproducible builds | Build determinism | Local: Python 3.12, PyQt6 present | HIGH | Lock file | CI | No | 10 |

---

## E. References and licensing

| ID | Question | Why it matters | Current evidence | Conf. | Measurement required | Test method | Blocking? | Phase |
|---|---|---|---|---|---|---|---|---|
| UNK-REF-001 | Primary-source confirmation of MS3 trigger/config/log details | Reference doc accuracy | Community-level only | LOW | Read the MS3 manual/licence directly | Documentation review | No | 0 (open) |
| UNK-REF-002 | ECUMaster public architecture/feature detail | Competitive benchmark | Marketing only | LOW | Product documentation review | Documentation review | No | 0 (open) |
| UNK-REF-003 | MoTeC M1/i2 public architecture detail | Professional-workflow benchmark | Marketing only | LOW | Documentation review | Documentation review | No | 0 (open) |
| UNK-REF-004 | NXP MC33813 / alternative smart-driver part availability and pricing | Power stage | Datasheet only | LOW | Supplier quotes | Purchasing | No | 1 |
| UNK-REF-005 | Confirm FDCAN instance count and features on the exact chosen STM32 part | CAN bus count drives the product | Vendor collateral inconsistent across sources | MEDIUM | Datasheet + HAL header inspection | Read the exact datasheet revision | **Yes** — pin budget | 1 |

---

## F. Closing procedure

1. Whoever resolves an item updates the row with the **evidence** (measurement record ID, or
   document URL + page/date), changes `Conf.`, and adds the date.
2. Items that become facts generate: a constant with `source` in the code/calibration
   schema (spec §102), a `DERIVED`/`MEASURED` provenance tag, and a test that would catch a
   regression.
3. An item is never closed by "we picked a number".

## UNK-GT-020 - Gear position source

| Field | Value |
|---|---|
| Status | UNKNOWN - blocks gear-dependent features |
| Owner | ENGINEER |
| Needed by | Engine braking table row, gear strategy table, gear-corrected traction control, pit-limiter per-gear targets |

There is no gear input channel in the current channel map (`RC_IN_*` stops at wheel
speeds), so nothing reports which gear is selected. Options, none chosen:

- A gear-position switch on a discrete input.
- Derivation from wheel speed and engine speed using the known final drive and ratios,
  which is ambiguous in a slip and needs the clutch state to resolve.
- A CAN gear report, if the vehicle has one.

Until one of these is chosen and measured, `rc_control_cycle` passes a neutral gear 0 to
the performance module. The affected features are declared unsupported rather than driven
from an invented value.
