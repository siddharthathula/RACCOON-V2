# ACTUATOR SPECIFICATION

**Document ID:** ACT-001 · **Status:** PHASE 1

Every actuator has, without exception: current limit, thermal limit, timeout, declared
default state, declared fault state, a hardware disable path, and a hardware capability
declaration (spec §50). An output that cannot state these does not ship.

---

## 1. Injector drivers (§28)

| Aspect | Requirement |
|---|---|
| High-Z (saturated) | native support |
| Low-Z (peak-and-hold) | only with a driver stage that provides peak current, hold current, and current limit; declared per hardware profile |
| Peak-and-hold control | hardware current limit with a programmable peak phase; the ECU commands "open" and the driver manages the waveform |
| Minimum pulse | enforced in the driver or the timer |
| Maximum pulse / duty | hardware ceiling; exceeding it is a DTC + a protective cut |
| Pulse overlap | structurally impossible by design (each injector is an independent channel; a *shared* driver cannot be shared between overlapping pulses) |
| Open-load / stuck-on detection | feedback where available; a stuck-on injector is a DTC and a fuel cut |
| Thermal | driver overtemperature → cut, DTC |
| Hardware disable | a fault input clears the output without CPU involvement |

**Design decision (`UNK-HW-005`):** evaluate a smart-driver IC (e.g. NXP MC33810-class)
against a discrete solution. The published smart-driver features that matter to us:
4.5–36 V VPWR, 4.5 A injector current limit, ignition gate pre-driver with **spark-duration
feedback**, max-dwell timer, soft shutdown, and per-channel diagnostics. Spark-duration
feedback is the decisive feature: it converts "we commanded a spark" into "a spark voltage
event was observed", which is the measurement the safety case and the dyno analyser need.

---

## 2. Ignition drivers (§30, §33)

| Aspect | Requirement |
|---|---|
| Coil topologies supported | smart coil, logic-level coil, dumb coil + external IGBT, COP, wasted spark pairs |
| Flyback clamp | mandatory on any dumb-coil path |
| Dwell control | ECU-computed for dumb coils; driver-internal for smart coils |
| Max dwell | hardware-enforced, independent of the CPU |
| Current sense | primary current sense for dwell regulation and over-current protection |
| Spark detection | via a primary-side divider (a 9:1 divider is the published reference for a smart driver) — gives a *verified* spark event |
| Coil open/short | detected where the topology allows |
| Over-temperature | cutoff + DTC |
| Hardware disable | yes |

**CDI:** not assumed, not supported without a validated profile entry. A CDI has its own
timing model and its own failure modes; treating it as a "coil" would be wrong.

---

## 3. Low-side outputs (§50)

Solenoids, relays, pump, boost, VVT, IAC PWM, purge, tach, shift light, warning, user
outputs.

Each declares:
```
current_limit        (A)          — enforced in the driver
thermal_limit        (°C)         — driver shutdown + DTC
timeout              (ms)         — maximum continuous ON, then forced off
default_state        (off)        — with the ECU unpowered
fault_state          (off)        — with a driver fault
duty_max             (%)          — for PWM outputs
diagnostics          (open/short/over-current where the topology allows)
hardware_disable     (yes)        — fault input clears it without the CPU
```

The ECU reports each output's live state, its measured current (where sensed), and its
fault status in the ECU health page (§83) and the diagnostics workspace (§82).

---

## 4. H-bridge outputs (stepper IAC, DBW)

- Two H-bridges, each independently current-limited and fault-flagged.
- **Stepper:** a bounded microstep table, a bounded current, and a **hard stall/timeout
  limit** — a stepper that is commanded forever is a thermal event, so there is a maximum
  energised time and a return-to-zero policy.
- **DBW:** a position controller with a *ramp-limited* command, dual-sensor plausibility,
  runaway detection (position not following the command), and a limp mode that reduces
  authority to a defined safe value. **If the hardware is not present, `CAP_DBW` is off and
  no workspace appears** (§38, §94).

---

## 5. High-side outputs

Main relay and fuel pump. Both must have a **defined state when the ECU loses control**:
a welded main relay is a common failure; the ECU therefore monitors the relay feedback
where available and reports it.

---

## 6. Timeouts and bounded actuation (§103)

| Output class | Timeout basis |
|---|---|
| Fuel pump | ON while the ignition key/run state is true; OFF otherwise; a maximum ON time with a defined consequence |
| Main relay | ON while the ECU is initialised and self-test passed; OFF on any fatal fault |
| Boost solenoid | PWM duty bounded; off on overboost, sync loss, or a closed-loop fault |
| VVT | duty bounded; off on fault; position feedback where available |
| IAC | duty bounded; off on sync loss or a stepper timeout |
| Taux / tach / shift light | duty bounded; default off |

**No output has an unbounded energised duration.** Every bounded value is a calibration
parameter with a maximum, a default, and a source.

---

## 7. Output conflict detection

At configuration validation time, the validator must detect and refuse:
- two functions assigned to the same physical output,
- a fuel-pump output and a VVT output on the same channel,
- an ignition output and a low-side solenoid output on the same channel,
- a function that requires a capability the hardware profile lacks (§98),
- an injector and an ignition output on a channel whose driver cannot do both.

At runtime, a per-channel "owner" model means a non-owning subsystem cannot write the
channel — a structural fix for a whole class of §125 "output conflict" defects.

---

## 8. Actuator test matrix

| ID | Test | Pass condition |
|---|---|---|
| A1 | Injector open/close edges | within the measured bound |
| A2 | Injector current limit (low-Z) | limits at the calibrated value; no driver damage |
| A3 | Injector duty ceiling | hardware cuts at the ceiling; DTC |
| A4 | Stuck-on injector | detected (where feedback exists); fuel cut; DTC |
| A5 | Driver overtemperature | cut; DTC; recovery after cooldown |
| A6 | Hardware disable on fault | output cleared **with the CPU halted** (fault injection) |
| A7 | Dwell with a real coil | measured dwell within tolerance; no coil damage at the max-dwell limit |
| A8 | Max dwell with the CPU halted | coil de-energised by hardware |
| A9 | Spark detection | spark event observed and logged; mismatch between command and spark raises a DTC |
| A10 | Low-side over-current | limit; shutdown; DTC |
| A11 | Output timeout | forced off after the timeout |
| A12 | Stepper stall | current limit + timeout; no thermal damage |
| A13 | DBW runaway | detected; limp mode entered |
| A14 | Power loss during any actuation | deterministic off state; reset cause logged |
| A15 | Output conflict | refused at validation with a specific reason |
