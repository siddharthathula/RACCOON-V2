# CAN SYSTEM SPECIFICATION

**Document ID:** CAN-001 · **Status:** PHASE 1

---

## 1. Scope

CAN is used for four distinct purposes, each with different latency and safety
characteristics. Keeping them separate is a design decision, not an implementation detail.

| Purpose | Bus | Latency class | Failure impact |
|---|---|---|---|
| ECU ↔ tuner (tuning, logs, updates) | CAN1 | low priority, best effort | none on the engine |
| Vehicle / OEM integration (dash, ABS, body) | CAN1 or CAN2 | normal | depends on the vehicle function |
| Sensor/adapter inputs (external wideband, EGT, gear) | CAN2 | normal, with **timeout policy** | a timeout must have a defined fallback |
| ECU output (dash data, torque requests from a TC module) | CAN1/2 | normal | loss ⇒ defined degradation |

**Design rule:** a CAN frame may be an *input* to the control loop only if it has an
explicit timeout, a fallback value, and a validity flag. A CAN frame may never be on the
real-time path in a way that can block it.

---

## 2. Bus configuration

| Parameter | Value | Note |
|---|---|---|
| Controllers | 2 × FDCAN (per `IO_MATRIX.md`; exact count for the chosen part → `UNK-REF-005`) | |
| Bitrate | user-configurable; 250/500/800/1000 kbit/s nominal + FD data rates | Persisted per project |
| Termination | hardware, in the harness and/or the ECU | Documented in the install manual |
| Mode | ISO mode (no ISO-TP by default) or a bounded diagnostic transport profile | A bounded transport profile carries the §framing protocol over CAN |
| Filters | hardware acceptance filters per bank + a software filter list | Reduces ISR load |
| TX | a scheduled queue with priority classes and a **bounded** retry count | Never blocks the control loop |
| Bus-off | detected, counted, reported; recovery policy is explicit and bounded | Bus-off during a run must not stop the engine |

---

## 3. Signal database (user-configurable, §51)

A CAN database entry is data, editable in the GUI, validated in the ECU:

```yaml
id: 0x2A5
direction: rx
name: external_wideband_lambda1
start_bit: 0
length: 16
endianness: little        # or big
signedness: unsigned
factor: 0.0001
offset: 0.0
unit: lambda
timeout_ms: 200
fallback: null            # → invalid, not a made-up value
validity:                 # a signal is only usable when the source is alive
  min_raw: 0
  max_raw: 65535
```

Supported: `length` 1–64, signed/unsigned, scale/offset, unit, timeout, fallback, and an
optional **unit conversion + provenance** label so a lambda is never displayed as AFR by
accident.

**Validation refuses:** overlapping bit ranges in the same frame, a signal exceeding the
frame length, a timeout below the bus's worst-case latency, and a fallback that would make
an invalid signal look valid.

---

## 4. Standard ECU output frames

The ECU broadcasts a fixed, documented set so third-party dashes and loggers work without
a custom database:

| ID | Period | Content |
|---|---|---|
| `0x100` | 10 ms | engine state: rpm, tps, map, coolant, iat, lambda, fuel pressure, oil pressure, battery |
| `0x110` | 10 ms | per-cylinder: injection pulse, ignition angle, dwell, knock retard, trim |
| `0x120` | 20 ms | status: sync state, mode, armed, safety actions, torque arbitration trace |
| `0x130` | 50 ms | event/fault summary and DTCs |
| `0x140` | 100 ms | health: ISR latency, CPU load, injector duty, dwell headroom, flash/log health |
| `0x150` | on request | diagnostics payload |
| `0x200+` | 20 ms | vehicle data: speed, wheel speeds, gear, throttle, brake, clutch |

A published **DBC** is generated from the schema so third parties can integrate. IDs are
in a reserved RACCOON range; the user may relocate them.

---

## 5. Per-cylinder lambda on CAN

If `CAP_MULTIPLE_LAMBDA`, channels 1–4 are broadcast individually with their validity and
sensor status. A tuner may combine this with a sampling window: the ECU reports which
cylinder's exhaust each channel represents, so the GUI never mislabels a per-cylinder
value.

---

## 6. External wideband via CAN

Supported. Requirements:
- the channel declares a **controller status** and a **sensor status**, not just a value;
- a controller that reports "sensor not ready" is `invalid`, and the ECU does not enter
  closed loop;
- timeout ⇒ invalid within the declared window, with a defined fallback (usually: hold the
  last target and raise a DTC, or revert to open loop — chosen by calibration, not guessed).

This is the primary wideband path in V2; the native CJ125 path (§21) is the upgrade.

---

## 7. CAN and the real-time rule (§104)

| Guarantee | Mechanism |
|---|---|
| A CAN interrupt cannot delay an injector/ignition edge | Interrupt priority below the control loops; timer hardware produces the edges |
| A CAN transmit cannot block | Bounded queue with a drop-oldest/drop-newest policy per priority class and a hard cap on retries |
| A full bus cannot stall the control loop | TX is a background task; the control loop never calls a transmit function |
| CAN traffic cannot corrupt engine state | Every received signal is validated into a `sensor_value_t`; a bad frame is a fault, not a value |
| Bus-off recovery is deterministic | Bounded recovery attempts, then the channel is declared failed and a defined policy applies |

---

## 8. Tests

| ID | Test | Pass condition |
|---|---|---|
| C1 | Every database entry decodes correctly against a synthetic frame | exact expected value including sign/endianness |
| C2 | Invalid database (overlap, overrun, zero timeout) | refused at validation with a reason |
| C3 | Signal timeout | marked invalid within the declared window; fallback applied; DTC |
| C4 | Bus-off injection | detected; recovery policy followed; **engine unaffected** throughout |
| C5 | Flooded bus (TX and RX) | control loop timing unchanged (measured); queue bounded |
| C6 | Malformed frames at the bit level | rejected; no state change |
| C7 | Broadcast frame period accuracy | within the declared tolerance under load |
| C8 | Per-cylinder lambda labelling | correct cylinder association in the GUI and the DBC |
| C9 | Third-party dash integration | an external tool using the published DBC reads correct values |
