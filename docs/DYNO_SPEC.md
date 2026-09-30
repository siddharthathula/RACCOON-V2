# DYNO, DRAG, TRACK AND PERFORMANCE ANALYSIS SPECIFICATION

**Document ID:** DYNO-001 · **Status:** PHASE 1

Rule that governs this entire document: **compute only what the data supports; never
invent a metric** (§68, §77, §114). A metric whose inputs are absent is displayed as
`unavailable` with the reason, not estimated.

---

## 1. Metric availability contract

```python
@dataclass(frozen=True)
class Metric:
    name: str
    value: float | None
    unit: str
    required_channels: tuple[str, ...]
    quality: str            # MEASURED | DERIVED | ESTIMATED | UNAVAILABLE
    confidence: float       # 0..1, with the dominant uncertainty named
    method: str             # the exact algorithm, recorded with the value
```

Every number displayed in any analyser is a `Metric`. The dashboard renders `None` as
`—` with a tooltip explaining the missing channel. This is what prevents a drag dashboard
from showing a fabricated 1/4-mile time when there is no wheel-speed channel.

---

## 2. Dyno analysis (§78, §113)

| Output | Definition | Required channels |
|---|---|---|
| Power | `P = T · ω`, ω from rpm | torque channel (dyno or calculated), rpm |
| Torque | dyno sensor, or `P/ω` | as above |
| Corrected power/torque | per the configured correction (temperature, baro, humidity) — **the correction factor is shown, never hidden** | temperature, baro, humidity if used |
| Run overlay | multiple runs on one axis set | per-run channel sets |
| Smoothing | configurable moving average or Savitzky–Golay, **with the window shown**; raw data always available | — |
| Annotations | user markers | — |
| Cylinder trims | from per-cylinder lambda/ignition channels | `CAP_MULTIPLE_LAMBDA` or per-cylinder ignition |
| Knock overlay | knock events plotted against (rpm, load) | knock channel |
| Run comparison | delta curves between two runs | — |

Rules:
- The dyno's own calibration/verification status is displayed. A power number from an
  uncalibrated cell is labelled `UNVERIFIED`.
- Smoothing never replaces raw data; the raw series is one click away.
- No automatic "optimisation" of fuel or ignition to maximise power (§113). The analyser
  reports; the tuner decides; protection always applies.

---

## 3. Drag analysis (§68, §111)

| Metric | Method | Required |
|---|---|---|
| Elapsed time | from the first motion event (wheel speed above a threshold) | speed |
| 60 ft, 330 ft, 1/8 mile, 1000 ft, 1/4 mile | distance marks from integrated speed | speed; **wheel circumference is a stated input** |
| 0–100 km/h, 0–60 mph, 60–100, 0–200 etc. | acceleration marks from speed | speed |
| Reaction time | requires an explicit reaction input (transponder/brake) | reaction channel; otherwise `unavailable` |
| Launch RPM | the RPM held at launch; the minimum over the hold window | rpm |
| Peak RPM | max rpm in the run | rpm |
| Shift times | between rpm/load discontinuities with the strategy identified | rpm, gear or shift events |
| Distance | `∫v dt` with a stated integration method and stated circumference | speed |

Honesty rules:
- The distance marks depend on **tyre circumference**, which is a user input with a
  stated value and an uncertainty. The result carries that uncertainty.
- 1/8 mile = 201.168 m; 1/4 mile = 402.336 m; 330 ft = 100.584 m; 1000 ft = 304.8 m.
  These are exact, not rounded marketing numbers.
- Reaction time is shown **only** when a real reaction input exists. It is never
  back-computed from the log, because that is not what reaction time means.
- If the run ended before a mark, that mark shows `not reached`.

---

## 4. Track analysis (§79, §112)

| Output | Method |
|---|---|
| Lap times | from a track map (sector boundaries) or a lap-detection channel |
| Sector times | segment deltas between boundaries |
| Lap comparison | best/worst/last/selected, plus a delta trace |
| Speed / RPM / gear / throttle / λ / temperature / oil pressure / traction traces | direct channel plots |
| Braking | only if a braking input exists; otherwise absent |
| Shift events | from the event log, with the shift time derived from the rpm discontinuity |
| Temptation to invent | none: a track with no GPS or no lap-detection channel cannot produce lap times, and the UI says so |

---

## 5. Vehicle performance metrics (§77)

| Metric | Formula | Quality notes |
|---|---|---|
| 0–X km/h | time from rest to X | rest = speed < 0.5 km/h sustained |
| X–Y km/h | time between two speed thresholds | same run |
| Elapsed time | see §3 | — |
| Shift time | from rpm discontinuity / gear channel | states the detection method |
| Launch RPM | see §3 | — |
| Peak RPM | max | — |
| Distance | integrated | states circumference |

Every metric records its detection method so two metrics computed on the same run by
different methods can be reconciled.

---

## 6. Power/torque from a *calculated* source

If no dyno torque channel exists, power/torque may be derived from the ECU's air/fuel
state:

```
fuel_mass_rate  from injector pulse widths × flow
air_mass_rate   from MAP, rpm, and VE
indicated power ∝ air_mass_rate / AFR_effective
```

This is **indicated**, not brake, and it is labelled `DERIVED (indicated)` with the
assumptions listed: no pumping loss model, no friction model, no volumetric-efficiency
uncertainty band. It must never be presented as a measured crank power figure. Showing it
as a relative comparison across runs is legitimate; showing it as an absolute horsepower
number is not.

---

## 7. Tests

| ID | Test | Pass condition |
|---|---|---|
| D1 | Synthetic power/torque run | values match to numerical tolerance |
| D2 | Distance integration | exact on a constant-speed synthetic run |
| D3 | Missing channel | metric is `unavailable` with the missing channel named; **no value is shown** |
| D4 | Incorrect circumference | result changes proportionally; the input is displayed with the result |
| D5 | Partial run | unreached marks show `not reached` |
| D6 | Reaction time without a reaction channel | `unavailable`, never estimated |
| D7 | Smoothing | window is displayed; raw series retrievable; result matches a reference implementation |
| D8 | Correction factors | applied factors are displayed with the corrected and uncorrected values |
| D9 | Run comparison | deltas match a hand-computed reference |
| D10 | Indicated power labelling | the UI and any export show `DERIVED (indicated)` with assumptions |
| D11 | Playback determinism | replaying a run reproduces every metric exactly |
| D12 | No silent estimation | a test asserts that no code path produces a value from missing inputs |
