# TRIGGER AND ENGINE POSITION SPECIFICATION

**Document ID:** TRG-001 · **Status:** PHASE 1 design (Phase 2 implementation)

---

## 1. Scope

The trigger subsystem converts a stream of timestamped position-sensor edges into a
trusted **engine angle model**. Everything else in the ECU — fuel, ignition, torque
management, logging, the GUI's live cell trace — depends on this being correct. It is the
highest-risk subsystem in the product and is designed accordingly.

The firmware never assumes a raw crank signal can be connected directly to an MCU
(spec §11). The signal chain is:

```
sensor → protection (ESD/surge/reverse/clamp) → conditioning (VR: differential amp +
adaptive threshold + zero crossing; Hall: hysteresis + filter) → MCU timer capture
(32-bit, 1 µs) → trigger decoder → engine angle
```

The firmware's input contract is therefore: **a clean digital level with a known polarity
and a bounded edge rate.** Anything failing that contract is a hardware problem and is
reported as such, not worked around in software.

---

## 2. Signal conditioning requirements (hardware contract)

| Requirement | Value | Rationale |
|---|---|---|
| Differential VR input | yes | Common-mode noise on a single-ended pickup is the dominant failure mode in a motorcycle harness |
| Adaptive peak threshold | threshold = 1/3 of previous cycle peak (MAX9926 behaviour, datasheet-verified) | Rejects broken-tooth and off-centre-wheel false triggers |
| Zero-crossing output | yes | The most reliable phase marker; the tooth centre |
| Threshold recovery watchdog | 85 ms internal (MAX9926), plus firmware timeout | Recovers pulse detection after intermittent sensor connection |
| Operating frequency | 5 Hz – 25 kHz (MAX9926 rated) | Covers stall to the design overspeed limit |
| Input series resistance | 10 kΩ per input | Limits ESD-diode current if the sensor pulse exceeds VCC |
| Input filter capacitor | across inputs, per datasheet | Bandwidth limiting / EMI |
| Output | open-drain → pull-up to 3.3 V | Level-compatible with the MCU; polarity configurable in firmware |
| Hall inputs | configurable pull-up/pull-down, hysteresis, digital filter | Handles both Hall and switch-to-ground sensors |
| ESD | on every input | Field-replaceable harness, workshop environment |
| Negative voltage | clamp + series limiting | Reverse battery / miswiring |
| Surge | per ISO 7637-2 pulse requirements (thresholds to be set — `UNK-HW-007`) | Starter crank transient is the worst case |

**Unverified → `UNK-HW-003`:** the actual conditioner design is not yet made; these are
requirements, not a design.

---

## 3. Trigger profiles

A trigger profile describes a wheel configuration. It is *data*, selected by the engine
profile, and is independent of the ECU hardware.

```c
typedef struct {
    const char*  name;
    uint8_t      teeth;              /* nominal teeth per revolution on this wheel   */
    uint8_t      missing_teeth;      /* contiguous missing-tooth gap               */
    uint8_t      teeth_per_rev;      /* 1 = crank wheel, 2 = cam (half-speed) wheel  */
    int16_t      angle_of_tooth1;    /* crank degrees ATDC of tooth #1               */
    bool         is_crank;
    bool         direction_aware;    /* quadrant / dual-wheel direction detection   */
} trigger_profile_t;
```

Shipped profiles (initial set — deliberately generic, not OEM-specific):

| Profile | Teeth | Missing | Notes |
|---|---|---|---|
| `mt_36_1` | 36 | 1 | industry-standard 36-1 crank wheel |
| `mt_60_2` | 60 | 2 | 60-2 crank wheel |
| `mt_60_3` | 60 | 3 | |
| `mt_24_1`, `mt_24_2` | 24 | 1/2 | common on small motorcycle/starter rings |
| `mt_16_1` | 16 | 1 | |
| `even_24`, `even_36`, `even_60` | even | 0 | no missing tooth — needs a different sync mechanism (gap *detection*) |
| `cam_4_1`, `cam_3_1` | 3–4 | 1 | half-speed cam wheel with gap |
| `dual_wheel` | — | — | crank + cam, phase-locked |
| `quadrature` | — | — | direction from two channels |
| `custom` | — | — | user-defined tooth count/phase |

**Requirement:** a `custom` profile must be expressible for *any* wheel with a detectable
asymmetry or missing-tooth gap; if a wheel has neither, the design must fall back to
crank-only with a *stated* limitation (and refuse sequential operation), never invent a
phase (§12, §98).

---

## 4. Sync state machine

```
                ┌──────────────────────────────────────────┐
                │                                          │
   power-on ───▶│ NO_SIGNAL                               │◀─── tooth timeout,
                │                                          │      no edges at all
                ▼                                          │
          ┌───────────┐  first valid edge                  │
          │INITIALIZING├──────────────────────────────────┘
          │  measure period, seed filters, discard
          └─────┬─────┘
                │  period plausible
                ▼
          ┌────────────┐  start a pattern search
          │SEEKING_SYNC│  (expect gap / missing tooth)
          └─────┬──────┘
                │ candidate pattern found
                ▼
          ┌────────────────┐  confirm N times, or confirm with cam
          │SYNC_CANDIDATE  │
          └─────┬──────────┘
                │ confirmed
                ▼
          ┌──────────┐
          │  SYNCED  │──── single anomalous period ──▶ ┌──────────┐
          └────┬─────┘                                   │ DEGRADED │
               │                                         └────┬─────┘
               │ edges stop, or too many errors             │ recovered
               ▼                                              │
          ┌────────────┐                                     │
          │ SYNC_LOST  │◀────────────────────────────────────┘
          └─────┬──────┘
                │ fuel/ignition inhibited per safety policy
                ▼
          ┌────────────┐
          │RECOVERING  │  re-acquire; policy (crank vs run) is mode-dependent
          └─────┬──────┘
                │ recovered
                └──────────▶ SYNCED / SYNC_CANDIDATE

          any state ──▶ FAULT   (e.g. impossible period, overspeed, persistent
                               inconsistency beyond the retry budget)
```

**Every arrow above is a test case.** The state machine is a pure function of the event
stream so that it can be exhaustively tested on the host, including pathological streams
(missing tooth, false tooth, reverse, stall, restart, noise).

### 4.1 State semantics (what the ECU is *allowed* to do)

| State | RPM available | Injection | Ignition | Reason |
|---|---|---|---|---|
| `NO_SIGNAL` | no | off | off | no edges |
| `INITIALIZIZING` | provisional | off | off | one period is not a position |
| `SEEKING_SYNC` | provisional | off | off | no phase reference |
| `SYNC_CANDIDATE` | provisional | off (or cranking enrichment path if configured) | limited (cranking advance) | pattern seen but not confirmed |
| `SYNCED` | yes | **enabled** | **enabled** | confirmed |
| `DEGRADED` | yes, filtered | enabled, degraded policy | enabled | isolated anomalies |
| `SYNC_LOST` | decaying estimate | **off immediately** | **off immediately** | trust is gone |
| `RECOVERING` | provisional | off | off | re-acquiring |
| `FAULT` | no | off | off | hardware/protocol fault |

**Design stance:** in every state other than `SYNCED`/`DEGRADED`, fuel and spark are
*inhibited by default*. Cranking enrichment is available only in an explicitly configured,
separately-validated cranking path, and cannot coexist with a sequential injection
strategy without cam sync (because without cam sync the injector could be commanded into
the wrong stroke).

---

## 5. Engine position model

```c
typedef struct {
    uint32_t   rpm_x2;             /* filtered, x2 for resolution */
    uint16_t   crank_angle_deg10;  /* 0..3599 (0.1°) */
    uint16_t   cycle_angle_deg10;  /* 0..7199 (0.1°) */
    uint16_t   tooth_index;
    uint32_t   tooth_period_us;    /* measured */
    uint32_t   period_filtered_us; /* for prediction */
    uint8_t    sync_state;
    uint8_t    sync_confidence;    /* 0..100 */
    uint8_t    cylinder_phase;     /* 0..3, meaning depends on firing order */
    int32_t    accel_rpm_per_s;    /* filtered d(rpm)/dt */
    int32_t    decel_rpm_per_s;    /* filtered -d(rpm)/dt, for overrun/fuel-cut */
} engine_position_t;
```

**RPM estimation:** period-based, low-pass filtered in the *period* domain (constant-Q
behaviour is what you want near steady state), with an acceleration term from a
phase-locked tracker so that a sudden acceleration does not produce a large transient
error. Chosen over a simple moving average because an MA over 2–3 teeth is too noisy for
spark scheduling at low RPM and too lagged at high RPM.

**Synchronous 0.1° resolution** is chosen because at 12 000 rpm the crank turns 7200°/s;
0.1° = 13.9 µs, which is finer than the 5 µs scheduling error target would need, and
costs nothing. `0.1°` granularity is also what the GUI live-cell trace displays.

---

## 6. Acceleration, deceleration, and prediction

```
rpm_predicted(t) = rpm_now + accel · t ,   with accel clamped to ±ACCEL_MAX
                                   (ACCEL_MAX is a bounded, calibrated constant —
                                    no unbounded extrapolation, §103)
```

Rules:
- Prediction is only used for events **within the current inter-tooth window or the
  next window**; it is never used across a gap longer than `PREDICTION_HORIZON_US`
  (which is bounded and calibrated).
- During a *missing tooth* (which is 2–3× a normal period), prediction is used to
  interpolate the gap; the *scheduled* events inside the gap are re-anchored each tooth
  so that an acceleration error cannot accumulate across the whole gap.
- If `|accel|` exceeds the plausibility bound, prediction is disabled for that window and
  a diagnostic counter is incremented (`accel_out_of_bounds`). This is a fault signal, not
  a silent fallback.

---

## 7. Synchronisation sources, in order of authority

| Rank | Source | Sufficient for | Notes |
|---|---|---|---|
| 1 | Cam edge (half-speed wheel) + crank | full sequential, per-cylinder lambda, per-cylinder ignition | 720° phase established |
| 2 | Missing-tooth gap on the crank wheel | full sequential on a 4-stroke with a known firing order *if the gap position defines the cycle* | valid when the gap is in a fixed relation to TDC |
| 3 | Crank teeth only, no gap (even wheel) | RPM + **crank-synchronous** operation only; batch/semi-sequential at best | sequential injection requires a phase source → must be refused (§98) |
| 4 | Cam only (starter) | no meaningful RPM at start | starter-only aid |

**The system never fabricates a phase source** (§12). If the configuration requests
sequential operation and no rank-1/2 source exists, validation fails with the reason.

---

## 8. Noise, false teeth, and missing teeth

| Disturbance | Detection | Response |
|---|---|---|
| **False tooth** (extra edge) | period far below the expected minimum for the current RPM | edge rejected; count incremented; if the rate exceeds the budget → `DEGRADED`, then `SYNC_LOST` |
| **Missing tooth** (absent edge) | period far above the expected maximum, or a gap matching the configured missing-tooth length | treated as a gap; phase model interpolates; tooth counter skips the missing indices |
| **Noise burst** | multiple edges inside the input filter window; impossible periods | all edges in the window rejected; adaptive hardware threshold helps at the source |
| **Trigger reversal** | period sequence inconsistent with the pattern, or direction detection (if available) contradicts | sync lost; policy per mode |
| **Impossible period** | outside `[PERIOD_MIN, PERIOD_MAX]` | edge rejected, counter incremented |
| **Overspeed** | period below `OVERSPEED_PERIOD_MIN` | immediate `FAULT`; fuel and spark inhibited **by hardware** as well as software |
| **Stall** | no edges for `STALL_TIMEOUT_MS` | `NO_SIGNAL`; fuel/ignition inhibited |
| **Restart from stall** | edges resume | re-enter `SEEKING_SYNC`; cranking path if configured |

All thresholds are **calibration parameters with units, sources, defaults, and bounds**
(§102) — not constants buried in code.

---

## 9. Software input filter

Speeduino's public documentation describes a time-based filter with OFF/LITE/MEDIUM/
AGGRESSIVE levels. We adopt the *idea* with different defaults and different semantics:

- **Default: LITE.** A filter that rejects a real tooth is far more dangerous than one that
  passes a noise edge, because a missed tooth desynchronises phase.
- The filter window is derived from the *current* period (bounded, e.g. 20–35% of a period),
  so it scales with RPM instead of being a fixed time.
- Filter aggressiveness is exposed in the GUI, and the *rejection count* is a logged
  channel, so a tuner can see whether the filter is doing damage.

---

## 10. Trigger logging

A dedicated tooth log, independent of normal telemetry:

| Field | Rate | Purpose |
|---|---|---|
| tooth index | per edge | reconstruct the wheel |
| timestamp (1 µs) | per edge | period reconstruction |
| period (µs) | per edge | acceleration |
| sync state | per edge | why a tooth was accepted/rejected |
| cam edge marker | per edge | phase |
| rejection reason code | on rejection | diagnostics |
| filtered/accepted flag | per edge | filter audit |

This is a ring buffer written at the edge rate (that is the *only* thing allowed to write
at that rate, and only into RAM), drained to storage by the low-priority loop. The GUI
renders it as the trigger oscilloscope (§128.4) with the **decoder state overlaid** — the
part that tells the user *why* a tooth was accepted.

---

## 11. Test matrix (Phase 2 exit criteria)

| # | Test | Pass condition |
|---|---|---|
| T1 | `mt_36_1` clean, 50 → 12 000 rpm, monotonic | reaches `SYNCED`; RPM tracks within the filter's stated error; tooth count correct |
| T2 | `mt_60_2` clean | as T1 |
| T3 | even wheel, no gap | `SYNCED` not claimed; correct refusal message; RPM still valid |
| T4 | deceleration sweep to stall | no false sync; `NO_SIGNAL` on stall; clean restart |
| T5 | stall → restart | re-acquires; no wrong-phase arming |
| T6 | single missing tooth injected | detected, phase maintained, no sync loss |
| T7 | false tooth injected | rejected; rejection logged; no phase error |
| T8 | noise burst (random edges) | bounded rejection count; state degrades then recovers |
| T9 | tooth timeout | `SYNC_LOST` within the specified time; fuel/ignition inhibited within the specified deadline |
| T10 | resynchronisation | re-acquires within N revolutions, from a *known* starting condition |
| T11 | overspeed | `FAULT`; hardware inhibit verified on the bench |
| T12 | trigger reversal | detected; no incorrect injection |
| T13 | 1, 2, 3, 4 cylinder × 5 injection strategies × 3 ignition strategies | every combination produces a consistent schedule with no channel conflicts |
| T14 | acceleration extremes (step from 1 000 → 11 000 rpm) | prediction bounded; no stale-phase scheduling; errors within `UNK-FW-001/002` |
| T15 | VR conditioner sweep 5 Hz → 25 kHz with noise | hardware acceptance test, not firmware |

**Every state transition is asserted explicitly** (spec §10: "every state transition must
be testable"). The test harness drives the decoder through a scripted edge stream and
asserts the exact state sequence — this is a *sequence* assertion, not a smoke test.
