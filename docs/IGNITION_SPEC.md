# IGNITION SPECIFICATION

**Document ID:** IGN-001 · **Status:** PHASE 1

---

## 1. Separation of concerns (spec §30, §100 rule)

```
ignition_command   (degrees BTDC, per cylinder)      ← software, calibrated, logged
        ↓
coil_driver_mode   (smart coil | logic coil | dumb coil + external IGBT | COP)
        ↓
dwell_control      (smart driver internal | ECU computed)
        ↓
hardware protection (max dwell, current limit, spark-duration feedback)
```

The firmware produces a **command**; the driver topology determines what the command
means. CDI compatibility is **not assumed** and is not supported without a validated
hardware profile entry.

| Mode | Firmware responsibility | Required hardware |
|---|---|---|
| Smart coil / COP | on/off command with a coil-on timeout | coil with internal dwell/current limit |
| Logic-level coil | on/off command | external logic-level igniter |
| Dumb coil + external IGBT | **computed dwell** | IGBT stage with flyback clamp and current sense |
| Wasted spark | paired on/off | paired coils, correct polarity |

---

## 2. Ignition map and corrections (§31)

```
base = table(rpm, load)

corrections (each named, bounded, logged):
    IAT          (usually negative when intake air is warm)
    ECT          (usually negative when the engine is warm)
    BARO         (altitude)
    BATTERY      (low voltage ⇒ reduced dwell capability ⇒ retard)
    ETHANOL      (charge cooling ⇒ positive or negative per calibration)
    GEAR         (per-gear strategy)
    TPS          (load-following)
    TRANSIENT    (knock-adjacent events)
    KNOCK        (dynamic, per cylinder)
    LAUNCH       (launch retard)
    TRACTION     (TC retard)
    ENGINE_BRAKING (advance on overrun)
    USER_TRIM    (per-cell user offset)

final = clamp(base + Σcorrections, MIN_ANGLE, MAX_ANGLE)     ← hard envelope
```

**Order matters and is fixed.** The user trim is applied *before* the safety clamp, so a
user can never push past the hard envelope by combining trims. The safety envelope is
applied last, always.

---

## 3. Dwell (§33)

```
dwell_target = dwell_table(battery, rpm)          [ms]
dwell_actual = dwell_target + dwell_comp(high_rpm)
dwell_actual = clamp(dwell_actual, DWELL_MIN, DWELL_MAX)
```

**Hard limits — enforced in hardware as well as software:**

| Limit | Enforcement |
|---|---|
| `DWELL_MAX` | hardware max-dwell timer (smart driver) or an independent timer that de-energises the coil; software also clamps |
| `DWELL_MIN` | software clamp (below this, a coil will not reach current) |
| Coil-on timeout | a **separate, independent** watchdog de-energises the coil regardless of the main control logic |
| Over-current | hardware current limit aborts the charge |
| Thermal | driver overtemperature → de-energise, DTC |
| Timer overflow / wrap | wrap-safe arithmetic + an assertion |

**No unbounded coil energisation is possible** (§33, §103). This is a hardware property,
not a software promise: if the CPU hangs mid-dwell, the hardware timer still ends it.

**Dwell table provenance:** the battery × rpm table is a *calibration*, and the underlying
physics is primary (coil primary inductance, resistance, target current, spark plug
energy). `SOURCE` must be either `MEASURED` (a current probe on a real coil) or a
`CALCULATED` model with its parameters listed. An unmeasured default table is shipped
flagged as `UNVERIFIED` and the ECU raises an informational DTC when it is used in a
non-dyno mode. See `UNK-GT-006`.

---

## 4. Per-cylinder ignition (§32)

Each cylinder has independent:
- commanded advance,
- knock retard (accumulated and released independently),
- dwell (same base, independently bounded),
- an individual "no spark" bit for cut strategies.

Per-cylinder state is a small array indexed by cylinder, and the safety envelope is applied
per cylinder. A cylinder in cut produces **no** ignition event — it is a suppression at the
scheduler, not a "0°" command (0° is a valid advance and must never be overloaded to mean
"cut").

---

## 5. Knock control (§34)

```
knock_signal  →  band-pass (5–20 kHz)  →  envelope detect  →  peak-hold
             →  per-cylinder association (from the firing angle)
             →  intensity metric
             →  retard request (per cylinder, decaying recovery)
             →  torque manager
```

Requirements:
- A knock window per cylinder, opened around the expected combustion window.
- A **noise floor** estimate so a disconnected sensor does not masquerade as "no knock"
  (or as constant knock).
- Recovery: retard releases with a bounded rate; it does not snap back to full advance
  (that is how engines get hurt).
- Persistent knock → escalating action: retard → additional retard → cut → shutdown.
- **No knock claim without hardware.** If no knock sensor is fitted, `CAP_KNOCK` is off,
  the workspace is disabled in the GUI, and the ECU says so.

---

## 6. Ignition cut strategies (§43, §47, §48)

| Strategy | Action | Re-enable condition |
|---|---|---|
| Rev limiter | no spark above the limit | RPM below the limit minus hysteresis |
| Launch | retard, then cut at the target | gear/speed/torque condition |
| Quickshift | spark cut for the shift duration | shift complete (gear or rpm/tps condition) |
| Traction | retard proportional to slip | slip below the target |
| Anti-lag | timed cut sequence | sequence complete |
| Pit limiter | cut at a target speed/rpm | below the limit |
| Knock escalation | cut | knock resolved |
| Shutdown | full cut | manual re-arm |

**Rule:** every cut is a latched suppression in the scheduler with a generation counter and
a defined re-enable predicate. A cut can never be undone by a later calculation in the same
cycle (§125 adversarial: "ignition resurrection").

---

## 7. Ignition test matrix (Phase 5 exit)

| ID | Test | Pass condition |
|---|---|---|
| I1 | Commanded angle delivered | error ≤ the measured bound (`UNK-FW-002`) across the RPM range |
| I2 | Dwell delivered | error ≤ bound; measured with a current probe |
| I3 | Max dwell | hardware terminates at the limit even if software hangs (fault injection: halt the CPU) |
| I4 | Coil-on timeout | terminates; DTC |
| I5 | Battery voltage sweep | dwell compensates; ignition is retarded at low voltage per policy |
| I6 | Sync loss during dwell | coil de-energised immediately; no stored energy left in a coil |
| I7 | Rev limiter | no spark above the limit; no overshoot; correct hysteresis |
| I8 | Knock response | retard applied within the window; recovery bounded; persistent knock escalates |
| I9 | Per-cylinder knock | only the affected cylinder retards |
| I10 | Quickshift | spark cut for the specified duration; recovery per policy; no overlap with traction cut |
| I11 | 4-cyl wasted spark | correct pairing; correct polarity; no double-fire |
| I12 | Safety envelope | no combination of corrections + user trim exceeds the hard limit |
| I13 | Timer wrap | long-run behaviour identical to a short run (no wrap-dependent behaviour) |
| I14 | Ignition resurrection adversarial | with every cut strategy active simultaneously, a cut is never cleared by a non-owner |
