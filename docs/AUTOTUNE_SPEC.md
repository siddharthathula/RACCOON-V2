# AUTOTUNE AND IGNITION ANALYSER SPECIFICATION

**Document ID:** AUTO-001 · **Status:** PHASE 1

The autotuner is a **statistical instrument**, not an autopilot. Its output is a proposal
with evidence, confidence and risk. It never writes a safety-class item and it never
advances ignition from noisy data (§73, §74, §84, §115).

---

## 1. Sample admissibility

A sample is admitted to a cell's statistics only if **every** gate passes.

| Gate | Condition | Rationale |
|---|---|---|
| `G_SYNC` | sync state is `SYNCED` (not `DEGRADED`) | a degraded decoder means the cell may be attributed to the wrong RPM |
| `G_LAMBDA_VALID` | the measured lambda channel is valid and its controller/sensor status is good | a faulted sensor produces a confident wrong answer |
| `G_STEADY_RPM` | `|d(rpm)/dt|` below a threshold for the dwell time | acceleration changes the required fuel for the same cell |
| `G_STEADY_LOAD` | `|d(load)/dt|` below a threshold | transients are exactly what transient enrichment is compensating for |
| `G_NOT_TRANSIENT` | transient state inactive | otherwise the commanded fuel ≠ the steady-state fuel |
| `G_NO_CUT` | no decel cut, launch cut, shift cut, or fuel cut active | cut events have no meaningful fuel command |
| `G_NO_LIMITER` | not on the rev limiter or in a torque-reduced state | the torque manager is limiting torque, so the air/fuel pair is not representative |
| `G_WARM_ENOUGH` | engine temperature above a threshold | cold operation is dominated by enrichment and wall wetting |
| `G_SETTLED` | the cell has been continuously occupied for ≥ `SETTLE_TIME` | allows the closed loop and film model to settle |
| `G_SAMPLE_COUNT` | ≥ `MIN_SAMPLES` in the window | variance must be meaningful |
| `G_NO_KNOCK` | no knock event in the window | a knocking cell is a calibration defect, not a data point |
| `G_SENSOR_HEALTH` | all sensors feeding the cell are valid | — |

**Every rejection is counted and reported.** A tuner must be able to ask "why is cell 7×4
not learning?" and get an answer with counts per gate. This is a direct response to the
failure mode of commercial autotune tools, which silently ignore data.

---

## 2. The estimator

For each admitted sample in a cell we form the observation

```
ratio_i = measured_λ / target_λ          (dimensionless)
```

and, for speed-density, the fuel correction implied by that ratio:

```
correction_i = 1 / ratio_i
```

Because `ratio_i` for a speed-density VE table is defined at a fixed λ, the correction is
applied to the **fuel command**, not to λ.

Estimator properties:
- **Weighted** by sample quality `q_i ∈ (0,1]`, combining inverse variance and gate margin.
- **Trimmed**: the top and bottom `TRIM_FRACTION` (e.g. 10 %) of samples are discarded
  before averaging — a robust estimator, because a single mis-attributed sample in a
  16×16 table is a permanent calibration defect.
- **Recency weighted**: older samples decay with a half-life, so a cell tracks a real
  change (a jet, a leak, a different fuel) instead of averaging across it.
- **Bounded output**: `|correction − 1| ≤ MAX_CORRECTION` per cell per pass, and the total
  cumulative change per cell is bounded (`MAX_CUMULATIVE`).
- **Variance reported**, and a confidence derived from it.

```
confidence = f(n_effective, relative_variance, gate_coverage, stability)
             ∈ [0, 1]     shown before any change is applied
```

---

## 3. Output and application

```
┌────────────────────────────────────────────────────────────┐
│ VE AUTOTUNE PROPOSAL — cell (rpm 4200-4800, map 65-90)    │
├────────────────────────────────────────────────────────────┤
│ Data used      1 284 samples over 3 passes, 41 s           │
│ Excluded       transient 210 · unsteady rpm 88 · no valid │
│                lambda 12 · below temp 6 · knock 1         │
│ Observation     measured λ averages 3.4 % lean of target  │
│ Analysis       VE is too high; weighted trim 1.036,       │
│                σ 0.011, n_eff 1 102                        │
│ Recommendation  +3.6 % fuel in 4 cells (bounded ±1 %/pass)│
│ Confidence      0.91                                         │
│ Expected effect lean correction ~3.6 %, est. power +0.4 %  │
│ Risk            low; cells are in the mid-map steady region│
│                   not knock-limited, temp 92 °C             │
│                                                             │
│ [ Preview map diff ]  [ Apply selected ]  [ Discard ]       │
└────────────────────────────────────────────────────────────┘
```

- **Nothing is applied automatically.** The user previews a diff (heat map of proposed
  changes) and selects cells to apply.
- **Safety-class items are never in an applicable set.**
- The proposal is saved with the log it came from, so it is auditable: "this change was
  suggested from this data, with this confidence, on this date".
- Applying writes through the normal calibration transaction, so it is versioned and
  revertable.

---

## 4. Rejection-mode behaviour (must be demonstrated, not claimed)

| Situation | Required behaviour |
|---|---|
| Lambda sensor fails mid-run | the run stops, the reason is stated, partial statistics are discarded or clearly marked |
| Engine is bouncing on the limiter | all samples rejected with a `G_NO_LIMITER` count; no nonsense correction |
| A cell is occupied only in transients | reported as `insufficient settled data`, with a suggestion to run a steady sweep |
| Variance is very high | confidence low; the cell is excluded; the user is told the cell is unstable, not that the fuel is wrong |
| Correction would exceed the bound | clamped **and the clamp is stated** |
| A knock event occurs in a cell | the cell is flagged for ignition/knock review rather than fuel correction |

---

## 5. Ignition analyser (§74)

Purpose: help a tuner find where timing is limited — and *prove* it.

Inputs: ignition angle, rpm, load, λ, knock, torque (when available).

Capabilities:
- **Timing sweep** analysis: across a sweep, plot advance vs. torque; the point where
  torque stops increasing is the *measured* limit, not an assumed one.
- **Dyno-run comparison**: overlay two runs' timing, knock, and torque; identify the
  difference.
- **Knock correlation**: correlate knock events with (cell, cylinder, load, temperature)
  and show the cells at risk.
- **Timing map visualisation**: render the proposed advance change as a diff map.

**Non-negotiable rule:** the analyser **never automatically advances ignition**. It may
propose an increase with evidence, and it must state the evidence, the confidence, and the
risk. It must also state when the data is insufficient (e.g. no torque channel, no wideband,
knock not measured). A tool that advances timing from a noisy trace is dangerous, and this
is a design prohibition, not a preference.

---

## 6. AI tuning assistant (§84, §115) — advisory only

```
It may:  explain a log · find bad data · flag suspicious cells · identify lean/rich
         regions · identify repeated knock · detect unstable sensors · compare runs ·
         recommend further testing

It may NOT: write a safety item · write ignition advance without a timing-sweep basis ·
            apply a change without an explicit user action · fabricate a number
```

Every recommendation is rendered as a card with:

```
OBSERVATION · DATA · ANALYSIS · RECOMMENDATION · CONFIDENCE · RISK
```

plus the concrete channels and time range it used, so the user can open that exact region
of the log. A recommendation that cannot cite its data is not shown.

The assistant runs on the PC. It has no write path to the ECU except the normal,
authenticated, user-initiated calibration transaction. Data boundary: log data stays local
unless the user explicitly enables an external model, and the setting is visible.

---

## 7. Tests

| ID | Test | Pass condition |
|---|---|---|
| A1 | Gate coverage | each gate rejects a constructed violating sample and counts it |
| A2 | Rejection reporting | every gate's count is visible in the UI |
| A3 | Statistical correctness | on a synthetic cell with a known 5 % offset, the estimator converges to 1.05 within its stated tolerance |
| A4 | Outlier robustness | a 20 % outlier sample does not move the estimate beyond the bound |
| A5 | Bound enforcement | no cell ever exceeds `MAX_CORRECTION` or `MAX_CUMULATIVE` |
| A6 | Confidence monotonicity | more samples and lower variance ⇒ higher confidence |
| A7 | No auto-apply | a run that completes with no user action writes nothing |
| A8 | Safety exclusion | no proposal contains a safety-class item |
| A9 | Auditability | a proposal can be re-derived from the stored log |
| A10 | Ignition non-advance | the analyser cannot produce an "apply advance" action without a sweep basis |
| A11 | Insufficient data | with no torque channel, the analyser says so instead of guessing |
| A12 | AI card completeness | every card has all six required fields and cites its data range |
