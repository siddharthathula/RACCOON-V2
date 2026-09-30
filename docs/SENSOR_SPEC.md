# SENSOR SPECIFICATION

**Document ID:** SENS-001 · **Status:** PHASE 1

---

## 1. The sensor value contract

Every sensor, regardless of type, produces the same structure. This is what makes
validation, fallback, logging and GUI rendering uniform.

```c
typedef struct {
    float    value;            /* engineering units                            */
    uint32_t timestamp_us;     /* engine time or system time, stated            */
    uint32_t sequence;         /* monotonic per sensor                          */
    uint8_t  valid;            /* value is usable                               */
    uint8_t  fresh;            /* updated within the freshness window          */
    uint8_t  range_valid;      /* inside the calibrated range                  */
    uint8_t  plausible;        /* passes rate / cross-checks                   */
    uint8_t  fault;            /* bitmask of reason codes                      */
    float    fallback;         /* value substituted on invalid                 */
    uint8_t  using_fallback;   /* explicit, logged                             */
} sensor_value_t;
```

**Invariant (§16):** a stale sensor value must never silently remain valid. `fresh` is
computed from a *sequence* advance and a timestamp comparison, not from a "value looks
fine" heuristic.

---

## 2. Fault taxonomy

| Code | Detection method | Typical cause |
|---|---|---|
| `OPEN` | reading outside the calibrated open-circuit band | disconnected sensor, blown fuse |
| `SHORT` | reading at/near the rail | shorted sensor or wiring to 12 V/5 V/ground |
| `FROZEN` | sequence does not advance / value delta ≈ 0 over a window | failed sensor, frozen ADC |
| `STALE` | `timestamp` older than the freshness limit | DMA stall, task starvation, disconnected |
| `RATE` | change rate above the physical maximum | wiring fault, EMI, wrong calibration |
| `RANGE` | outside the calibrated physical range | wrong sensor fitted, miscalibration |
| `INCONSISTENT` | cross-check with a paired sensor fails | TPS1/TPS2, MAP vs TPS rationality |
| `ADC_FAULT` | ADC error flags / impossible code pattern | hardware fault |
| `DMA_FAULT` | DMA error / missed half-transfer | driver or bus fault |
| `RATE_LIMIT` | open-loop rate limiting was active | protection active, must be visible |
| `NO_REFERENCE` | the 5 V reference is out of tolerance | **power-supply fault that would otherwise silently corrupt every 5 V sensor** |

Every one of these has a **defined action**: substitute a declared fallback, mark invalid,
raise a DTC with a freeze frame, and record the fault. There is no path where a faulted
sensor is used as if valid.

---

## 3. Transfer functions (§19, §80)

| Type | Implementation | Notes |
|---|---|---|
| Linear (voltage) | `v = a·raw + b` | The default; `a`,`b` from a **calibration** (not a datasheet guess) |
| Linear (ratiometric) | `v = raw/ADC_MAX · v_ref` | Uses the *measured* 5 V reference, not a nominal one |
| NTC | Steinhart–Hart (full B equation) or a user table | Needs R25 and β, both `SOURCE`-tagged; `UNK-GT-009` |
| RTD | linearisation polynomial, where hardware supports it | cold-junction compensation required |
| Polynomial | 2nd/3rd order, coefficient-bounded | Used for pressure/linear-thermistor hybrids |
| Lookup | monotonic table with interpolation and extrapolation clamps | Values outside the table are flagged, not silently clamped |
| Switch/threshold | hysteretic digital level | For switches and pressure switches |
| Custom | user-supplied coefficients, bounded and range-checked | Never executed as code from the calibration blob (no scripting in the ECU) |

Each function is a pure, host-testable function with a test at the range endpoints, at
mid-scale, and at the open/short codes.

---

## 4. Analog front end requirements

```
sensor → series R (10 kΩ) → RC filter (fc chosen per sensor class) → ADC pin
              + clamp to 5 V/3.3 V
              + ESD
              + (5 V sensors) 5 V reference, monitored
```

- Sampling is **phase-aligned to the crank** (triggered from a timer) so that load and
  speed are read at a consistent crank angle. This removes a whole class of tuning
  confusion.
- Dual sampling mode for redundant pairs (TPS1/TPS2), with a bounded phase offset
  between the two ADC instances.
- Every channel has a **hardware** clamp: a bad harness must not be able to exceed the
  MCU pin limits.
- Effective resolution: 12-bit native; hardware oversampling is acceptable and its latency
  is accounted for in the freshness window.

---

## 5. Digital inputs (§15)

```c
typedef struct {
    active_polarity_t polarity;   /* active-high / active-low                  */
    pull_t            pull;       /* pull-up / pull-down / none (hardware)     */
    uint16_t          debounce_ms;
    uint16_t          filter_us;  /* glitch filter, separate from debounce      */
    uint16_t          timeout_ms; /* no-transition timeout                     */
    fault_action_t    fault_action;/* what the ECU does when faulted           */
    uint8_t           value;
    uint8_t           raw;        /* post-filter, pre-debounce, for debugging  */
    uint8_t           fault;
} digital_input_t;
```

Fail-safe rules:
- **Kill switch:** engine control must not arm while active; loss of the signal in a
  configured way must also inhibit (the choice is a documented, deliberate calibration,
  not a default guess).
- **Side stand / brake / pit limiter:** the fault action is explicit per input.
- Debounce is a real filter with a bounded window; it is not a single sample.

---

## 6. Frequency inputs

Wheel speed, MAF (frequency mode), flex fuel, EGT via a frequency converter: each has a
frequency range, a minimum period (to reject noise), a timeout, and a plausibility check
against a second source (e.g. wheel speed vs. vehicle speed derived from the gearing).

---

## 7. Freshness and rate limits (bounded, always)

| Sensor class | Freshness limit | Rate limit (per second) |
|---|---|---|
| TPS | 20 ms | 200 %/s |
| MAP | 20 ms | 500 kPa/s |
| ECT/IAT | 2 s | 20 °C/s |
| Oil pressure | 100 ms | 10 bar/s |
| Fuel pressure | 100 ms | 10 bar/s |
| Oil temperature | 2 s | 20 °C/s |
| Lambda | 40 ms (closed loop) | — |
| Wheel speed | 200 ms | — |
| Battery | 1 s | 20 V/s |

All are **calibration parameters with bounds**, not code constants (§102). The rate limit
is a *limiter with a logged flag*, so a real fast transient (a backfire, a valve event) is
detectable and not silently smoothed away.

---

## 8. Cross-checks (plausibility relationships)

| Pair | Check | Action on failure |
|---|---|---|
| TPS1 vs TPS2 | `|Δ| ≤ limit` (and rate of change agreement) | Invalidate both (DBW) or take the safe average (cable throttle) — the policy is explicit per configuration |
| MAP vs TPS | rationality (monotonic relationship within a band) | Flag, do not invalidate automatically (a misfire legitimately breaks it) |
| ECT vs IAT | ECT ≥ IAT − margin when the engine is warm | Flag implausibility |
| Front vs rear wheel | speed agreement within a band at low slip | Reduce TC confidence, do not act on garbage |
| Engine RPM vs vehicle speed vs gear | kinematic consistency | Derive/validate gear, raise a DTC if inconsistent |
| Lambda vs target (during steady state) | deviation within a band | Closed-loop correction; out of band → fault |

**Deliberate non-action:** a rationality check that flags a condition is *not* a
validity check. Validity is structural (open/short/stale/range); rationality is
diagnostic. Conflating them produces false shutdowns, which is the classic failure of
production ECUs.

---

## 9. Sensor test matrix (Phase 3 exit)

| ID | Test | Pass condition |
|---|---|---|
| S1 | Open circuit (each channel) | detected within the timeout; fallback used; DTC |
| S2 | Short to 5 V / 12 V / ground (each channel) | detected; no MCU damage (hardware test) |
| S3 | Frozen value | `FROZEN` within the window; invalid; DTC |
| S4 | Stale (no DMA update) | `STALE` within the freshness limit |
| S5 | Out of range | `RANGE`; invalid |
| S6 | Impossible rate | `RATE_LIMIT` active and flagged; not silently smoothed |
| S7 | ADC error flag | `ADC_FAULT`; invalid; safe fallback |
| S8 | DMA error / missed transfer | `DMA_FAULT`; invalid; no stale value used |
| S9 | 5 V reference drift | `NO_REFERENCE`; all dependent sensors invalidated |
| S10 | Dual-channel rationality | mismatch detected; policy applied as configured |
| S11 | Steinhart–Hart accuracy | within tolerance at −40, 0, 25, 100, 150 °C |
| S12 | Transfer-function boundary | correct at the endpoints; no discontinuity |
| S13 | Filter/rate-limiter flag visibility | a limited value is distinguishable from a true value |
| S14 | Recovery | a restored sensor returns to valid without an ECU reset |
