# ENGINE CONTROL SPECIFICATION

**Document ID:** ECS-001 · **Status:** PHASE 1

---

## 1. Engine configuration (the universal abstraction)

No engine-specific code exists anywhere in the core. An engine is *data*.

```c
typedef struct {
    /* geometry — sufficient to compute a volumetric model */
    uint8_t  cylinder_count;          /* 1..4                                  */
    float    bore_mm;
    float    stroke_mm;
    float    compression_ratio;
    float    rod_ratio;               /* UNK-GT-019 if unknown -> defaults flagged */
    float    displacement_cc;         /* derived, or authoritative if provided    */
    float    clearance_volume_cc;     /* if known; else derived from CR + swept  */

    /* layout */
    crank_layout_t   crank;           /* single | parallel_180 | 270 | 360 | inline4 | custom */
    float            crank_firing_angles_deg[4];  /* custom layouts                */
    uint8_t          firing_order[4];              /* user-defined                  */
    engine_cycle_t   cycle;           /* FOUR_STROKE_720 default                 */

    /* position sensing */
    trigger_profile_t crank_profile;
    trigger_profile_t cam_profile;    /* .teeth == 0 means no cam                  */
    float             cam_phase_offset_deg;      /* UNK-GT-002 if unknown         */
    uint8_t           cam_channels;   /* 0, 1, 2                                  */

    /* actuators */
    uint8_t  injector_count;          /* 1..8                                     */
    uint8_t  ignition_count;          /* 1..8                                     */
    injection_mode_t  injection;      /* sequential | semi | batch | staged       */
    ignition_mode_t   ignition;       /* sequential | wasted_spark | paired | smart_coil
                                         | dumb_coil_external_driver             */
    uint8_t  staged_injector_index[4];/* secondary injector per cylinder         */
    ignition_pair_t   ignition_pairs[4];

    /* strategies */
    fuel_strategy_t   fuel;           /* speed_density | alpha_n | maf | hybrid   */
    lambda_strategy_t lambda;         /* off | narrowband | wideband_external
                                         | wideband_native                        */
    idle_strategy_t   idle;           /* off | pwm_iac | stepper | dbw | ignition  */
    load_sensor_t     load_sensor;    /* map | tps | maf | hybrid                  */

    /* environment */
    float  fuel_stoich_afr_gasoline;
    float  fuel_stoich_afr_ethanol;
    float  fuel_density_g_per_cc;
    float  fuel_ethanol_fraction;     /* 0..1, or sensor-driven                 */
} engine_configuration_t;
```

**Invariant:** `cylinder_count` ∈ [1,4]. Out-of-range is rejected at validation, never
clamped silently.

**Derived at load time** (and cross-checked): displacement, swept volume per cylinder,
per-revolution volumetric flow, stroke frequency, compression-clearance volume.

---

## 2. Engine state (published to every consumer)

```c
typedef struct {
    engine_position_t  pos;            /* RPM, angles, sync                        */
    uint32_t           sequence;        /* monotonic                                */
    uint32_t           engine_time_us;  /* integrated from the crank                */
    uint32_t           cycle_count;

    load_t             load;            /* value + source + confidence              */
    lambda_t           lambda[4];       /* per cylinder, with validity              */

    /* the command vector actually used (for observability) */
    float  inj_ms[8];                   /* final commanded pulse width per injector */
    float  ign_deg[8];                  /* final commanded advance per channel      */
    float  dwell_ms[8];

    /* correction vector — every term separately observable */
    correction_vector_t corr;           /* see §6                                   */

    uint32_t arb_trace;                 /* torque-manager arbitration bitmask       */
    uint32_t safety_actions;            /* which hard limits are active             */
} engine_state_t;
```

---

## 3. Control cycle ordering

Executed once per 720° (four events per crank revolution for a 4-stroke), or on a bounded
2 kHz fallback when a decoder provides no 720°-aligned event (e.g. a 2-tooth crank-only
decoder at low RPM).

```
 1. publish/refresh sensor frames          (bounded)
 2. sensor validation & plausibility      → substitute or invalidate
 3. baro / MAP differential
 4. load computation (strategy-selected)
 5. fuel characterisation (ethanol blend)
 6. target lambda (table × fuel × gear × mode × corrections)
 7. air mass (mass balance, temperature/baro corrected)
 8. fuel mass → injector model (dead time, battery, pressure, min/max)
 9. transient (AE, wall wetting, tip-in/out, decay)
10. closed-loop correction (per channel, per cylinder)
11. per-cylinder trim
12. injection strategy assignment (sequential/semi/batch/staged) → per-injector ms
13. ignition base map + corrections (IAT/ECT/BARO/BAT/ETH/GEAR/TPS/transient)
14. knock evaluation → per-cylinder retard
15. dwell computation (battery × rpm) + bounds
16. ignition strategy assignment → per-channel deg
17. performance features issue torque requests
18. torque manager arbitration → allowed scale / ignition request / fuel request
19. HARD SAFETY ENVELOPE (final, non-bypassable)
20. schedule events into the scheduler with generation numbers
21. publish EngineState (double buffer + generation)
```

Steps 1–17 are *requests*. Steps 18–19 are *authority*. Only step 20 creates hardware
intent. This split is what makes the safety case arguable.

---

## 4. Scheduling: angle + time, anchored to a real event

Because the position sensor gives events at known angles rather than continuous position
(the central architectural insight from public research), scheduling is:

```
schedule(angle_θ after the current event E):
    period_at_θ   = predicted tooth period at θ
    t_offset      = θ / (360 / (rpm_pred / 60))
    target_tick   = tick(E) + t_offset
    arm hardware compare at target_tick
```

Rules:
- Every event carries a **deadline** (`target + DEADLINE_US`). Expiry is an explicit,
  counted fault with a defined safety action, not a silent late pulse.
- Events are re-planned once per cycle. A plan is a *generation*; anything older is
  discarded (no stale execution, no resurrection).
- Coil charge is scheduled as "dwell_ms before the spark angle", which may place it
  *before the current event* — handled by explicit wrap-safe arithmetic and a "schedule
  on the next cycle if in the past" rule that is a *bounded* decision, not a retry loop.

---

## 5. Multi-cylinder coverage

| Strategy | Requirement | Assignment rule |
|---|---|---|
| Sequential injection | cam sync (or crank gap that fixes the 720° phase) | one injector per cylinder, fired in the cylinder's intake stroke |
| Semi-sequential | full sync | pairs of injectors fired together, alternating halves per cycle |
| Batch | any position source | all injectors fired together at the batch angle |
| Staged | + `staged_injector_index[]` | primary always; secondary added by an activation predicate |
| Sequential ignition | per-cylinder coil | spark per cylinder at its own angle |
| Wasted spark | coils driven in pairs | both coils of a pair energised together; polarity per coil type |
| Paired | dumb coils + external driver | driver handles current; the ECU commands the pair |
| Smart coil / COP | built-in dwell + current limit | ECU commands on/off, not dwell duration |
| Dumb coil + external IGBT | ECU commands dwell | full dwell model required |

Firing angles come from the crank layout + firing order, so a 270° V-twin, a 180° twin, an
inline-4 and a triple all schedule correctly from the same code.

---

## 6. The correction vector (why the engine is "explainable")

```
base_map
+ sensor_correction      (IAT, ECT, BARO, BATTERY, ETHANOL)
+ strategy_factor
+ transient
+ closed_loop
+ cylinder_trim
+ performance_request    (launch/TC/shift)
× torque_allowance       (from arbitration)
→ hard_safety_envelope
```

Each term is a named `float` in `correction_vector_t`, transmitted in telemetry, and shown
in the GUI's live trace. This is the direct implementation of "show exactly what the ECU
thinks the engine is doing" (§128.2) and of "avoid multiple hidden enrichment paths"
(§26). **Any multiplier that is not in this vector is a bug.**

---

## 7. Modes (§48)

`STREET · SPORT · TRACK · DRAG · QUALIFY · RACE · DYNO · CUSTOM 1..3`

A mode selects *a calibration overlay* (maps and parameters), not a code path. Hard
protection is global and not overridable by a mode (§48). A mode switch is itself a
transaction (validated, CRC'd, committed) and takes effect at a defined point in the
control cycle — never mid-event.

---

## 8. Determinism requirements

| Requirement | Verification |
|---|---|
| L1 ISR WCET ≤ 6 µs, L2 control cycle ≤ 60 µs | cycle-counter instrumentation, worst case with 4 cyl + all logging |
| No allocation, no blocking, no unbounded loops in L1/L2 | static analysis + a build-time guard + code review |
| Hot region entirely in ITCM/DTCM | linker script check |
| Worst-case scheduling error ≤ 5 µs | HIL measurement (`UNK-FW-002/003`) |
| Deadline expiry has a defined, tested safety action | fault-injection test per event type |
| Behaviour identical for 1, 2, 3, 4 cylinders apart from the configuration | multi-cylinder test matrix (§89) |

---

## 9. Explicit non-goals in the engine core

- No per-model conditionals. No `if (engine == GT650)`.
- No screen/GUI calls, no logging-to-storage, no CAN in the control cycle.
- No unbounded extrapolation, retries, loops, or actuator durations.
- No feature may be enabled without the hardware capability that backs it.
