# FUEL SPECIFICATION

**Document ID:** FUEL-001 · **Status:** PHASE 1

---

## 1. Pipeline (single path, fully observed)

```
load  →  target λ  →  air mass  →  fuel mass  →  injector model  →  transient
      →  closed loop  →  cylinder trim  →  performance  →  torque  →  SAFETY  →  ms/stroke
```

Every stage is a separate, named, testable function. There are no hidden multipliers
(§26). The complete output — plus the entire correction vector — is a telemetry field.

---

## 2. Load strategies (§18, §25)

| Strategy | Equation | Required sensors | Notes |
|---|---|---|---|
| Speed density | `load = (MAP − baro)` | MAP + baro | The default for a motorcycle with a MAP sensor. Needs a valid baro; baro is learned at key-on. |
| Alpha-N | `load = f(MAP)` | MAP | Simpler, absorbs sensor drift, no baro needed. |
| TPS | `load = TPS × (RPM factor)` | TPS | Least accurate; a fallback only. |
| MAF | `load = mass flow` | MAF | Best absolute air measurement; sensor cost/availability. |
| Hybrid | weighted blend of MAP and TPS, blend factor calibrated | MAP + TPS | `MAP/blend` ratio per operating region; **not** a hidden average — a named, calibrated, logged term. |

All strategies output `(value, source, confidence)`. A strategy whose sensor is invalid
**fails to a declared fallback**, records the fallback in the correction vector, and sets
a DTC. It never silently produces a number.

---

## 3. Target lambda (§20–22, §35)

```
target_λ = table(rpm, load)
         × fuel_ethanol_blend      (E85 needs a different target; blending changes
                                     both lambda and energy density)
         × per_mode_target
         + temperature_correction  (cold = richer)
         + closed_loop_bias        (long-term learner offset, bounded)
```

Closed-loop targets are clamped by a **safety envelope on lambda** (§49): a lean-limit
exists and cuts/limits, it does not merely warn. The learner offset is bounded per cell,
per cylinder, and globally (§5 of this document) so a bad day cannot walk the entire table.

---

## 4. Fuel mass (mass balance)

```
air_mass = (MAP_abs − P_ambient) · V_eff / (R · T_intake)          [kg per cycle]

V_eff    = cylinder_swept_volume · (VE/100) · volumetric_efficiency_scaling(T, λ, fuel)

fuel_mass = air_mass / (target_λ · fuel_stoich_afr)                  [kg fuel per cycle]

fuel_mass += transient_addition                                     [kg]
```

Every term is a named function with a unit, a source, and a test:
`air_mass()` (ideal gas law with a documented compressibility/gamma correction),
`volumetric_efficiency()` (VE table, interpolated bilinearly with axis validation),
`stoich_afr()` (from the ethanol blend).

**No hard-coded density, AFR, or "magic" R value.** All come from the fuel
characterisation block with `SOURCE` provenance (§102).

---

## 5. Injector model (§28, §29)

```
ms = (fuel_mass / (flow_kg_per_s · pressure_comp · battery_comp)) · 1000
   + dead_time_ms(battery)
   + small_pulse_compensation     (only above the min-pulse threshold, never below)
ms = clamp(ms, min_pulse_ms, max_pulse_ms)
```

| Parameter | Unit | Source class | Bound |
|---|---|---|---|
| `flow` | g/s or cc/s @ reference pressure | **MEASURED** (bench flow test) or datasheet | — |
| `reference_pressure` | kPa abs | datasheet or measured | — |
| `dead_time` | ms | **MEASURED** (dead-time curve vs battery) | monotonic, bounded |
| `min_pulse` | ms | measured | ≥ dead time |
| `max_pulse` | ms | derived from duty ceiling and period | < 100% duty always |
| `battery_comp` | curve | **MEASURED** | bounded |
| `pressure_comp` | curve | measured | bounded |

**Small-pulse compensation** is applied *only* above the minimum-pulse threshold and is
**disabled by fuel cut** — it cannot resurrect a cut event (§7).

**Duty ceiling:** `max_duty` is enforced in hardware as well as software. Exceeding it is a
DTC + a defined protective action (cut the affected injector), not a warning.

---

## 6. Transient (§26, §27)

| Term | Trigger | Model | Bound |
|---|---|---|---|
| TPS acceleration enrichment | d(TPS)/dt above a threshold | extra pulse with a **decay time constant** | maximum added pulse, maximum time |
| MAP acceleration enrichment | d(MAP)/dt above a threshold | as above | as above |
| Wall wetting | any pulse | a film state variable that decays with τ, reducing the required additional pulse after a wet period | τ bounded, film mass bounded |
| Tip-in | throttle opening above a threshold | additive or multiplicative term | bounded |
| Tip-out | throttle closing below a threshold | pulse reduction toward min pulse | never below min pulse, and **cut wins** |
| Cold correction | ECT below threshold | multiplicative enrichment curve | bounded, decays as ECT rises |
| Hot correction | ECT above threshold | derate | bounded |
| Idle enrichment | closed loop off, low load | fixed/mult model | bounded |

**Rules:**
- Every enrichment term has a maximum, a decay, and a log channel. A term with no bound is
  a prohibited defect.
- There is exactly **one** enrichment accumulator; multiple independent enrichers cannot
  stack silently.
- On fuel cut, the accumulator is reset (not carried) and cannot re-energise below the
  reintroduction RPM.

---

## 7. Fuel cut precedence (hard rule, §27)

```
DECEL / OVERRUN CUT  ──▶  min-pulse clamp  ──▶  enrichment  ──▶  closed loop
        (highest precedence; applied as a HARD GATE at the output stage)
```

The fuel-cut decision is made once, in a dedicated arbiter, and applied at the output
stage *after* every other calculation. Nothing downstream of that gate can re-open the
injector within the same event. Reintroduction requires **all** of:
RPM above the reintroduction threshold **and** the hysteresis timer elapsed **and**
throttle/MAP above the threshold **and** the clutch/gear condition satisfied.

This rule is implemented as a *latched gate with a generation counter*, and is directly
testable: inject a min-pulse request, an enrichment request, and a closed-loop request
during a cut and assert zero output current at the driver.

---

## 8. Closed loop (§20–22)

| Sensor | Method | Authority | Rules |
|---|---|---|---|
| Narrowband | switching voltage, threshold-crossing period, duty-based average | limited | **Never treated as a wide-range AFR.** Closed loop is either on/off with a target band, or a slow trim. Heater control is hardware-gated. Warm-up: no closed loop before the sensor is hot. |
| Wideband (external, analog) | controller linearises the sensor; ECU reads λ | good | Controller status and sensor status must both be valid |
| Wideband (external, CAN/serial) | as above + controller diagnostics | good | Timeout → invalid, with a defined fallback |
| Wideband (native CJ125) | direct, with heater control and status | best | Only if the CJ125 is physically present (`CAP_WIDEBAND`) |
| Per-cylinder | one channel per cylinder where physically valid | best | **Never inferred from a single common-exhaust sensor without a validated methodology** (`UNK-FW-009`) |

**Loop stability:** the loop bandwidth is deliberately limited (time constant >> sensor
response, and << the transient time constant). Gains are calibrated; a stability margin
must be measured in SIL/HIL before the loop is enabled (`UNK-FW-010`). A controller is
never shipped with an unmeasured gain.

---

## 9. Long-term (4D) learner

Optional, gated, and bounded:

- Per `(rpm, load, mode, fuel)` cell, per cylinder.
- Learns only from **valid, settled, non-transient** samples.
- Bounded: `|trim| ≤ LEARN_TRIM_MAX` (e.g. ±20 % as a default, configurable within
  bounds), per-cell rate limit, and a global drift alarm if the *average* trim exceeds a
  band (which indicates a sensor or fuel problem, not a calibration drift).
- Disableable per cell, per engine, globally.
- Every learned change is logged with its reason.
- On `LEARN_TRIM_MAX` being reached, the cell freezes and raises a DTC — it never
  continues to walk.

---

## 10. Staged injection (§29)

Activation predicate (a named, calibrated, logged function) combines: rpm, load, TPS,
primary duty, boost, temperature. The split fraction is a parameter. Both injectors are
subject to the same min/max pulse, duty ceiling, and fuel-cut gate.

---

## 11. Flex fuel (§35)

```
ethanol_fraction = sensor-derived or user-set
stoich_afr   = blend(gasoline_stoich, ethanol_stoich, ethanol_fraction)
fuel_density = blend(...)
injector_comp = blend(...)          (flow is mass-based, so the fuel *quantity* changes)
lambda_target = blend(...)          (ethanol needs a different target for the same
                                     combustion stability)
ignition_blend = blend(...)         (ethanol charge cooling → more advance typically)
```

Every blended quantity is a named function with a test at 0 %, 50 %, 85 %, and 100 %
ethanol. **Never** a linear interpolation of a non-linear quantity without stating the
model (`UNKNOWN_REGISTER` `UNK-SW-*` / `FUEL_SPEC` test IDs F-20…F-24).

---

## 12. Fuel test matrix (Phase 4 exit)

| ID | Test | Pass condition |
|---|---|---|
| F1 | Dead-time curve across battery | monotonic; bounded; matches measurement within tolerance |
| F2 | Min-pulse clamp | never below min; the clamp is logged |
| F3 | Max-pulse clamp | never above max; duty ceiling respected |
| F4 | Battery sag during injection | pulse width widens by the calibrated amount; no command outside bounds |
| F5 | Fuel-pressure drop | compensation applied; low-pressure protection engages below the threshold |
| F6 | Acceleration enrichment | adds only within bounds; decays with the specified time constant; visible in the log |
| F7 | Deceleration cut | injector off; **stays off** despite min-pulse and enrichment requests; reintroduction conditions honoured |
| F8 | Rev limiter | fuel cut at the limit; no overshoot beyond the configured hysteresis |
| F9 | Staged activation | second injector only active inside the predicate; correct split |
| F10 | Closed-loop stability (narrowband) | no oscillation; measured stability margin |
| F11 | Closed-loop stability (wideband) | as F10, plus sensor-fault behaviour |
| F12 | Lambda sensor unplugged | invalid within the timeout; defined fallback; DTC |
| F13 | Per-cylinder trim | only active where a valid per-cylinder channel exists |
| F14 | Ethanol sweep | stoich, density, target, ignition blend all monotonic and bounded |
| F15 | 1–4 cyl × 4 injection strategies | correct assignment, no channel conflict, no missed event |
| F16 | Learner bounds | trim cannot exceed the bound; freeze + DTC at the bound |
| F17 | Lean safety limit | cuts/limits regardless of target lambda and learner state |
| F18 | Memory fault in a table | detected by CRC/ECC; falls back to a safe configuration; never runs on a corrupt table |
