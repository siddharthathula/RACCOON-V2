# TORQUE MANAGEMENT SPECIFICATION

**Document ID:** TQM-001 · **Status:** PHASE 1

The torque manager is the **single coordination point** between everything that wants to
change what the engine does (spec §40). Nothing else in the firmware is allowed to
modulate final fuel or ignition authority directly.

---

## 1. Inputs (requesters) and outputs

```
requesters                          torque_manager                     outputs
──────────                          ──────────────                     ───────
rider request (TPS/pedal/gear)  ──┐
launch control                   ──┤
traction control                 ──┤    (a) collect
quickshift / shift cut           ──┤    (b) resolve by fixed priority
rev limiter                      ──┤    (c) emit ONE command per event
knock retard                     ──┤    (d) log the arbitration
thermal protection               ──┤    (e) never exceed a hard limit
engine protection                ──┤    (f) never reduce a hard limit
boost protection                 ──┤
sync-loss inhibit                ──┤
sensor-invalidity fallback       ──┤
oil / fuel pressure protection   ──┘
                                                                 ──────▶ allowed_torque_scale
                                                                          ignition_request
                                                                          fuel_request
                                                                          arbitration_trace
```

---

## 2. Arbitration rules (each is a test)

**R1 — Monotone de-authority within a cycle.**
A request may only *reduce* authority. Authority granted in cycle *n* cannot be spent in
cycle *n+1* without re-arbitration. There is no "saved" authority.

**R2 — Fixed reduction priority.**
If two requests disagree, the *stronger reduction* wins, by this table (order is
normative, not incidental):

| Rank | Requester | Class |
|---|---|---|
| 1 | Sync-loss inhibit | immediate, hardware-backed |
| 2 | Hard engine protection (RPM, lean, oil, thermal, fuel pressure, battery) | immediate |
| 3 | Knock escalation | fast |
| 4 | Lean safety limit | fast |
| 5 | Thermal derate | slow |
| 6 | Rev limiter | immediate at the limit |
| 7 | Traction control | continuous |
| 8 | Shift / quickshift | bounded duration |
| 9 | Launch control | bounded duration |
| 10 | Boost overboost protection | immediate |
| 11 | Sensor-invalidity fallback | immediate |
| 12 | Engine braking | additive (see §4) |
| 13 | Rider request | baseline |

**R3 — Ownership of cut.**
A *cut* is owned by exactly one subsystem (the one that created it) and can only be
cleared by that owner's re-enable predicate. No other subsystem may clear it. This
directly answers the spec's "cut decisions are final" requirement and the §125
resurrection concern.

**R4 — Hard limits are never lowered by arbitration.**
Arbitration scales *requests*. The hard safety envelope (§110) is applied afterwards and
is not an input to arbitration. Arbitration cannot raise a request above the envelope.

**R5 — Latching vs decaying.**

| Request | Latch? | Release |
|---|---|---|
| Sync loss | yes | only on re-sync |
| Lean limit | yes | only when lambda returns to a valid band |
| Low oil pressure | yes | only above the recovery threshold + hysteresis |
| Knock | no | bounded recovery rate |
| Traction | no | continuous |
| Rev limiter | no | hysteresis |
| Shift | no | duration-bounded |

**R6 — Traceability.**
Each arbitration emits: the winning requester, the applied scale, the ignition request,
and the fuel request, into the telemetry correction vector. A log must be able to explain
any single cylinder's behaviour.

**R7 — Bounded everything.** Every latching request has a maximum lifetime. A latch
without a release path is a prohibited defect (it would be a slow brick in software form).

---

## 3. Allowed torque → fuel/ignition translation

```
allowed_scale ∈ [0, 1]     (1 = full authority)

fuel_request      = min(base_fuel_request, allowed_scale · base_fuel_request)
                    = base_fuel_request · allowed_scale      (reduction only)

ignition_request  = base_ignition + knock_retard
                               + launch_retard
                               + traction_retard
                               + (1 − allowed_scale) · retard_equivalent
```

`retard_equivalent` is a *calibrated* mapping from lost torque authority to timing, not a
guess — the relationship between advance and torque is engine-specific and is why this
must be measured, not assumed.

Engine braking (§46) is the one *additive* requester: it increases retard and reduces
fuel below the idle target under overrun, bounded by a calibrated table in (gear, rpm,
speed, throttle, mode).

---

## 4. Drag and track strategies (§42, §111, §112)

```
DRAG:
  staging → launch (target rpm hold, retard, fuel trim) → per-gear torque target
         → shift strategy → traction → elapsed-time logging
  Wheelie mitigation only if the hardware supports the required input
  (no fabricated capability)

TRACK:
  traction + engine braking + flat shift + launch + thermal/oil protection
  + knock + lambda targets + per-gear torque + pit limiter
```

Both are **profiles over the same arbitration**, not separate code paths. A drag strategy
requests torque; it never bypasses protection.

---

## 5. Pit limiter (§47)

Target speed or RPM, per gear, with an activation switch and hysteresis. Implemented as a
requesting subsystem with a bounded, latching cut, owned by itself. A pit limiter that
cannot be disengaged is a safety defect, so the disengage path is defined and tested.

---

## 6. Test matrix (Phase 7 exit)

| ID | Test | Pass condition |
|---|---|---|
| TM1 | Single requester at a time | output matches the requester's expected effect |
| TM2 | All pairs simultaneously | the table's precedence is respected exactly |
| TM3 | Fuzz: random simultaneous requests over 10⁶ iterations | **no** case where a higher-rank reduction is lost, **no** cut is cleared by a non-owner, **no** unbounded output |
| TM4 | Sync loss during launch | sync-loss wins; launch cleared; no fuel/spark |
| TM5 | Knock during traction | knock wins on the affected cylinder; TC unaffected elsewhere |
| TM6 | Rev limiter during quickshift | the stronger reduction applies; the combined result is within the hard envelope |
| TM7 | Launch + TC conflict | defined arbitration; no oscillation; logged |
| TM8 | Latch without a release path | structurally prevented (test: no code path exists) |
| TM9 | Trace completeness | every telemetry frame can attribute its fuel/ignition command to a requester |
| TM10 | Engine mode switch mid-event | the switch takes effect at the defined point; no partial event |
| TM11 | Pit limiter engaged while at the limit | cut holds; disengage restores control |
| TM12 | Protection during an active *performance* mode | protection still wins (modes do not override protection) |

TM3 is the adversarial test that makes the arbitration table meaningful rather than
aspirational.
