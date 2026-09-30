# PRODUCT REQUIREMENTS

**Document ID:** PRD-001 · **Status:** PHASE 1

Derived from `v2.md`. Each requirement is given a stable ID so that the architecture,
the test plan, and the release checklist can reference it. Every requirement carries a
verification method — a requirement with no verification method is not a requirement, it
is a wish.

---

## 1. Product definition

A compact, universal, motorsport-grade **standalone** engine management system for 1–4
cylinder motorcycles, with the Royal Enfield Continental GT 650 as the first validated
application, plus a professional desktop calibration, logging and analysis application.

## 2. Requirement groups

### R-CORE — standalone engine control (§5)
| ID | Requirement | Verify |
|---|---|---|
| RC-1 | The ECU shall determine crank synchronisation, RPM, and engine phase without the OEM ECU | Engine test with the OEM ECU disconnected |
| RC-2 | The ECU shall compute fuel, injection timing, pulse width, ignition timing, dwell, rev limit, launch, quickshift, protection, lambda control and idle without the OEM ECU | As above, per feature |
| RC-3 | Loss of OEM communication shall not affect engine control | Test with the OEM bus disconnected |
| RC-4 | Any dependency on OEM data shall be limited to unrelated vehicle functions | Architecture review |

### R-UNIV — universal engine support (§6, §7, §93)
| ID | Requirement | Verify |
|---|---|---|
| RU-1 | The core shall support 1, 2, 3, and 4 cylinders | Multi-cylinder test matrix (§89) |
| RU-2 | No application-specific logic shall exist in the core | Code review + a grep gate for model names |
| RU-3 | Crank arrangements: single, parallel 180/270/360, triple, inline-4, custom firing angles | Scheduling tests per layout |
| RU-4 | Firing order shall be user-defined | Validation + tests |
| RU-5 | Injection: sequential, semi-sequential, batch, staged | Strategy matrix tests |
| RU-6 | Ignition: sequential, wasted spark, paired, smart coil, dumb coil + external driver | Strategy matrix tests |
| RU-7 | Engines shall be described by an engine profile (data), and the GT650 shall be one profile among several | Profile schema + tests |

### R-TRIG — position sensing (§8–§12)
| ID | Requirement | Verify |
|---|---|---|
| RT-1 | VR, Hall, and digital inputs; crank-only, crank+cam, single cam, dual cam | Trigger test matrix T1–T15 |
| RT-2 | A 10-state decoder with a confidence scalar | State-sequence unit tests |
| RT-3 | All listed disturbances must be detected with a defined response | T4–T12 |
| RT-4 | The firmware shall not assume a raw crank signal may connect to the MCU | Hardware contract + code review (no direct GPIO on the CKP path) |
| RT-5 | The firmware shall not fabricate a phase source | Validation refuses sequential without a phase source |

### R-SCHED — real-time scheduling (§13)
| ID | Requirement | Verify |
|---|---|---|
| RS-1 | Every event shall carry type, cylinder, angle, timestamp, deadline, priority, generation, state | Structure + tests |
| RS-2 | Duplicate, stale, and resurrected events shall be impossible | Property tests |
| RS-3 | Timer overflow and collisions shall be handled deterministically | Wrap + collision tests |
| RS-4 | Actuator edges shall be produced by timer hardware | HIL + code review |

### R-SENS — inputs and validation (§14–§19)
| ID | Requirement | Verify |
|---|---|---|
| RS-5 | Configurable analog input types with user transfer functions | Sensor tests |
| RS-6 | Every digital input shall have polarity, pull, filter, debounce, timeout, fault behaviour | Structure + tests |
| RS-7 | Every sensor shall carry value, timestamp, sequence, validity, freshness, range, plausibility, fault, fallback | Structure |
| RS-8 | A stale value shall never remain valid indefinitely | Sensor tests S3, S4 |
| RS-9 | The 5 V reference shall be monitored | Sensor test S9 |
| RS-10 | TPS: single/dual, calibration, plausibility, stuck detection; DBW dual-channel | Sensor + tests |

### R-FUEL — fuel (§20–§29, §35)
| ID | Requirement | Verify |
|---|---|---|
| RF-1 | Narrowband shall not be treated as a wide-range AFR | Code review + documentation |
| RF-2 | Wideband via external controller (analog/CAN/serial); native only with a validated controller | Capability gating + bench |
| RF-3 | Up to 4 lambda channels; per-cylinder AFR shall not be inferred from a common sensor | Architecture + `UNK-FW-009` |
| RF-4 | Fuel pipeline shall be the single documented path with an observable correction vector | Code review + telemetry test |
| RF-5 | VE tables 8×8 … 20×20 (and larger where memory allows) | Codegen + memory budget |
| RF-6 | Strategies: speed-density, alpha-N, MAF, blended, staged | Strategy tests |
| RF-7 | Transient enrichment shall be bounded and observable | Fuel tests F6, F7 |
| RF-8 | A fuel cut shall be final and shall not be resurrectable | Fuel test F7 (adversarial) |
| RF-9 | Injector: high-Z, low-Z with hardware, peak-and-hold, staged; duty and pulse protection | Actuator tests A1–A5 |
| RF-10 | Flex fuel: frequency, analog, or CAN; blended stoich, density, target, ignition | Fuel tests F14 |

### R-IGN — ignition (§30–§34)
| ID | Requirement | Verify |
|---|---|---|
| RI-1 | Ignition command shall be separated from coil driver hardware | Architecture |
| RI-2 | CDI shall not be assumed | Documentation + profile capability |
| RI-3 | Ignition map with all listed corrections, applied in a fixed order | Code review |
| RI-4 | The final command shall pass a non-bypassable hard envelope | Property test |
| RI-5 | Per-cylinder advance, retard, knock, dwell | Tests |
| RI-6 | Dwell calibration by battery × rpm with hardware-enforced maximum dwell | HIL with the CPU halted |
| RI-7 | Knock control only with a real hardware signal path | Capability gating |
| RI-8 | The analyser shall never automatically advance ignition | Code review + autotune test A10 |

### R-PERF — performance features (§36–§48, §111, §112)
| ID | Requirement | Verify |
|---|---|---|
| RP-1 | Boost, VVT, DBW shall be architected but disabled without hardware | Capability gating |
| RP-2 | A torque management core shall be the central coordination layer | Architecture + TM tests |
| RP-3 | Launch, drag, quickshift, auto-blip, traction, engine braking, pit limiter | Feature tests |
| RP-4 | Auto-blip shall not pretend a cable throttle is electronically controllable | Architecture: ignition/fuel strategy only without a DBW |
| RP-5 | Ten race modes; hard protection global | Mode tests |
| RP-6 | A cable throttle shall not be presented as DBW | GUI + capability |

### R-SAFE — protection (§49, §103–§106)
| ID | Requirement | Verify |
|---|---|---|
| RSA-1 | Independent hard limits for every listed quantity | Structure + property test |
| RSA-2 | Actions: WARNING, RETARD, ENRICH, LIMIT, CUT, SHUTDOWN | Structure |
| RSA-3 | Hard limits shall always win, including in performance modes | Property test |
| RSA-4 | No unbounded dwell, pulse, boost, RPM, memory, loop, actuator duration, retry, or blocking path | Code review + static analysis + fault injection |
| RSA-5 | Real-time control shall not depend on GUI, USB, radio, storage, filesystem, CAN, cloud, logging, or OTA | Architecture + HIL stall test |
| RSA-6 | Power loss shall produce a deterministic state | Power tests |
| RSA-7 | Signed firmware, authenticated calibration, protected bootloader, unique identity, controlled debug, no universal password | Security review + tests |
| RSA-8 | Development bypasses shall not ship | Build gate |

### R-IO — outputs and CAN (§50, §51)
| ID | Requirement | Verify |
|---|---|---|
| RIO-1 | Every output shall declare current limit, timeout, default state, fault state, and hardware capability | Structure + validator |
| RIO-2 | CAN 2.0 (+FD where supported), configurable bitrate, filters, TX scheduling, user signal mapping with all listed attributes | CAN tests |
| RIO-3 | CAN transmit shall be configurable only with explicit safety controls | GUI + validator |

### R-DATA — logging (§52–§54, §75, §76)
| ID | Requirement | Verify |
|---|---|---|
| RD-1 | All listed channels loggable at configurable rates | Log tests |
| RD-2 | Trigger/ignition logging with raw tooth timing, viewable in the application | Log + GUI tests |
| RD-3 | Event logging of all listed events | Log tests |
| RD-4 | Professional log viewer with the listed capabilities | GUI acceptance |
| RD-5 | Playback with play/pause/step/rewind/fast-forward, replaying state, cells, and faults | GUI acceptance |
| RD-6 | Logging throughput shall be measured, not assumed | HIL |

### R-CAL — calibration (§55–§57, §97, §98, §110)
| ID | Requirement | Verify |
|---|---|---|
| RCAL-1 | Every item shall carry name, description, type, unit, min, max, default, source, validation, dependencies, safety class | Codegen output audit |
| RCAL-2 | Transactions: READ/VALIDATE/STAGE/CRC/COMMIT/VERIFY/ACTIVATE with rollback | Calibration tests |
| RCAL-3 | No partial configuration shall ever be observable | Power-loss tests |
| RCAL-4 | Tune version control: save, duplicate, compare, rollback, branch, merge, export | Tests |
| RCAL-5 | A wizard shall not create an unsafe configuration silently | Wizard validation tests |
| RCAL-6 | The arming gate shall refuse with `ENGINE CONTROL NOT ARMED` and a reason | Test each condition |
| RCAL-7 | The calibration hierarchy shall be implemented in the order of §110 | Code review |
| RCAL-8 | No safety-critical value shall be an unexplained constant | Static check + schema audit |

### R-PROTO — protocol, update, security (§58–§60)
| ID | Requirement | Verify |
|---|---|---|
| RP-1 | The binary protocol shall have the specified packet structure and the listed functions | Protocol tests |
| RP-2 | Firmware update over USB/CAN with signature, hash, version check, rollback, recovery bootloader | PR6, PR7 + power tests |
| RP-3 | An interrupted update shall never brick the ECU | PR6 |
| RP-4 | The enumerated security features shall be implemented | Security review |

### R-GUI — application (§61–§74, §80–§84, §116–§120)
| ID | Requirement | Verify |
|---|---|---|
| RGUI-1 | The application shall be PyQt6 with the listed stack, never blocking the GUI thread | Threading checks + responsiveness test |
| RGUI-2 | The listed workspaces shall exist | UI acceptance |
| RGUI-3 | Dashboards shall be customisable (drag, resize, save, duplicate, delete, import/export) | Tests |
| RGUI-4 | Race, dyno, drag and track dashboards shall show only supported metrics | Metric contract test |
| RGUI-5 | The map editor shall support the listed operations | Tests |
| RGUI-6 | Live cell trace shall show rpm, load, cell, row, column, interpolation, value, with the active region highlighted | Tests |
| RGUI-7 | Map compare shall show absolute, percentage, changed cells, min/max, histogram | Tests |
| RGUI-8 | VE autotune shall use weighted statistics, reject the listed conditions, and show confidence, sample count, variance, correction before applying | Autotune tests A1–A9 |
| RGUI-9 | The interface shall be dark, dense, keyboard-first, and configurable | Design review + UX tests |
| RGUI-10 | Dangerous actions shall require explicit confirmation with consequences shown | UI tests |
| RGUI-11 | Full keyboard operation with the listed shortcuts | Keyboard test pass |
| RGUI-12 | The application shall be responsive during all listed concurrent activities | Responsiveness test |
| RGUI-13 | Every recommendation from the assistant shall carry the six required fields | Autotune test A12 |
| RGUI-14 | Offline mode shall provide the listed capabilities | Offline acceptance test |

### R-SIM — simulation, HIL, testing (§85–§91)
| ID | Requirement | Verify |
|---|---|---|
| RSIM-1 | A simulator shall speak the production protocol | PR10 |
| RSIM-2 | A crank simulator shall support the listed patterns and disturbances | Simulator tests |
| RSIM-3 | HIL shall generate and measure the listed signals and compare commanded vs measured | HIL report |
| RSIM-4 | The full control stack shall be tested for 1–4 cylinders and the listed strategies | Matrix |
| RSIM-5 | Fault injection shall cover the listed faults with deterministic recovery | Fault matrix |
| RSIM-6 | Real-time performance shall be measured, not claimed | HIL report |

### R-PROD — production, docs, honesty (§99–§107, §114, §126, §133)
| ID | Requirement | Verify |
|---|---|---|
| RPROD-1 | An automated end-of-line test shall produce a machine-readable report | EOL report |
| RPROD-2 | Production builds shall be deterministic and produce a manifest with version, commit, compiler, dependencies, hashes | Build output |
| RPROD-3 | Static analysis shall be clean with zero unexplained warnings | CI |
| RPROD-4 | All listed documents shall exist and be current | Documentation audit |
| RPROD-5 | Every unresolved question shall be in the unknown register | Register audit |
| RPROD-6 | Every performance claim shall be measured | Claim audit |
| RPROD-7 | No absolute certainty language shall be used; evidence labels shall be used instead | Editorial review |

---

## 3. Non-goals (deliberate, documented)

| Non-goal | Reason |
|---|---|
| An ECU touchscreen/dash in V2 | The specified product is a desktop application; a screen adds cost, heat and glare for no calibration benefit. Substituted by CAN/analog dash outputs |
| ASIL / road-homologation compliance | This is an aftermarket motorsport product. Recorded so nobody assumes it |
| Supporting > 4 cylinders | Outside the stated product range. The architecture does not preclude it; the product does not claim it |
| Reverse-engineering or bundling any reference ECU software | Licence and clean-room obligations (`THIRD_PARTY_LICENSE_REGISTER.md`) |
| Shipping an ECU for a specific engine before its trigger/sensors are measured | `UNK-GT-*` are blocking; the profile stays at the safest configuration the measured hardware allows |
| Copying a TunerStudio/MS log or ini as the native format | Legal + technical debt; legacy import is optional and separate |

---

## 4. Success criteria for V2

| Criterion | Measure |
|---|---|
| Universal | the same core passes the 1–4 cylinder × strategy matrix |
| Deterministic | measured worst-case ISR latency and scheduling error, published, with the bench configuration |
| Safe | every hard limit proven to beat every requester; every hardware protection proven to survive a halted CPU |
| Honest | every capability, performance number, and document status carries an evidence label |
| Usable | a professional tuner completes a full calibration session using only the keyboard, at 2 AM, on a dyno |
| Releaseable | clean build, no unexplained warnings, no GPL, deterministic artefacts with hashes and a manifest |
