# LOG FORMAT SPECIFICATION

**Document ID:** LOG-001 · **Status:** PHASE 1

---

## 1. Design goals

1. **Engine-time alignment.** Every sample is stamped with engine time, cycle count, and
   (where meaningful) crank angle. A log is valid even if the transport stalled.
2. **Self-describing.** Channel set, units, scale, offsets, ECU identity, firmware, engine
   profile, and calibration identity are stored *in the file*.
3. **Stable forever.** Old logs open in future versions (`UNK-SW-005`).
4. **Streaming and seekable.** A file can be tailed live and seeked instantly.
5. **Bounded.** Fixed-size records, no unbounded strings, no unbounded channel count at
   read time.

---

## 2. File structure

```
┌────────────────────────────────────────────────────────────────┐
│ HEADER (fixed 4 KiB)                                           │
│  magic 'RLOG' | format version | flags | creation metadata     │
│  ECU identity, serial, hardware profile, firmware build,        │
│  schema version, engine profile, calibration hash, channel count │
│  session notes, project name, operator, time base description   │
├────────────────────────────────────────────────────────────────┤
│ CHANNEL DESCRIPTORS (channel_count × 64 bytes)                 │
│  id, name, unit, data type, scale, offset, min, max,            │
│  group, provenance, flags (per-cylinder, diagnostic, …)         │
├────────────────────────────────────────────────────────────────┤
│ CHANNEL GROUPS / MAP TRACE DESCRIPTORS (variable, bounded)      │
│  e.g. active VE cell (row, col, value) per sample group         │
├────────────────────────────────────────────────────────────────┤
│ MARKERS (event records: sync loss, knock, cut, shift, …)       │
├────────────────────────────────────────────────────────────────┤
│ DATA BLOCKS                                                    │
│  block header: engine_time range, sample count, byte length,    │
│                CRC32                                          │
│  fixed-width samples, one per configured rate group            │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. Rate groups

Different data needs different rates. One flat rate is wasteful and one slow rate loses
the interesting parts.

| Group | Default rate | Content | Rationale |
|---|---|---|---|
| `TRIGGER` | per tooth | tooth timestamps, sync events | only meaningful per tooth |
| `FAST` | 1 kHz | knock envelope, spark current, injection current (if sensed), crank angle | combustion events |
| `CORE` | 200 Hz | rpm, tps, map, lambda, injection ms, ignition deg, dwell, corrections | the main tuning dataset |
| `SLOW` | 20 Hz | temperatures, pressures, battery, oil, mode, faults | slow-changing quantities |
| `EVENT` | on occurrence | sync loss/gain, knock escalation, overtemp, lean, overboost, rev limit, launch, shift, traction, sensor fault, watchdog, reset, brownout, calibration error | a sparse, queryable record |

The user configures groups; the ECU enforces a **total throughput ceiling** and reports if a
requested set would exceed it. The ceiling is *measured* (`UNK-HW-009`), not assumed.

---

## 4. Sample record

Common header (all groups):

```
engine_time_us   u32    integrated from the crank, monotonic
cycle_count      u32
flags            u16    cut / retard / fallback / limit active bits
```

Group-specific payload. Example `CORE` frame:

```
rpm_x2           u16
tps_pct_x100     u16
map_kpa_x10      u16
lambda_x1000     s16   (sentinel = invalid)
inj_ms_x10[cyl]  u16 × cylinder_count
ign_deg_x10[cyl] s16 × cylinder_count
dwell_us[cyl]    u16 × cylinder_count
corr_vector      f32 × N   (base, sensor, transient, closed loop, trim, torque, safety)
arb_trace        u32
```

Fixed width per frame ⇒ O(1) seeking, and the file can be memory-mapped and indexed
without parsing.

---

## 5. The live calibration trace (a RACCOON differentiator, §128.5)

For each logged sample group, the ECU records the *active* map cell:

```
active_map_id     u16   (e.g. VE table)
active_row        u16
active_col        u16
active_value      f32
interp_fraction   f32 x2
```

This means the log viewer can highlight the exact cell that produced the command, and
playback can show the calibration state at any instant. It is the single most useful
feature for diagnosing "why did the engine do that", and it costs a few bytes per sample.

---

## 6. Trigger log

A separate, dense format (§53):

```
tooth_index u16, timestamp_us u32, period_us u32, accepted u8, reason u8, cam_edge u8
```

Retained in a RAM ring and drained to storage. The GUI renders it as the trigger
oscilloscope with the decoder state overlaid.

---

## 7. Event log

Each event record:

```
engine_time_us, cycle_count, event_code, severity, cylinder/mask, value_1, value_2,
duration_us (if bounded), sync_state_at_event, active_mode, safety_actions_at_event
```

Events are also pushed live to the GUI so a fault appears in the UI *while it happens*
(§82, and the "fault explanation" differentiator §128.9).

---

## 8. Freeze frames

On a DTC, the ECU snapshots a `CORE` + `SLOW` frame plus the trigger log window around the
event, and stores it with the DTC. The GUI shows it beside the explanation.

---

## 9. Storage and wear

| Concern | Policy |
|---|---|
| Write endurance | A ring buffer over the log medium; the wear budget and remaining endurance are reported in the ECU health page |
| Write atomicity | Per-block CRC; a partially written block is discarded on read |
| Buffering | A RAM ring absorbs burst rates; if the medium cannot keep up, the oldest data is dropped **and the drop is recorded in the file header** (never silently truncated) |
| Power loss | The log is diagnostic data; losing the tail is acceptable, and is bounded by the ring size |
| Read | Memory-mapped, indexed by block |

---

## 10. Live streaming vs. file

Both use the same encoding. A live session streams blocks over the protocol; the identical
bytes can be written to disk. A tuner recording a session therefore gets the same file the
ECU would have produced.

---

## 11. Tests

| ID | Test | Pass condition |
|---|---|---|
| L1 | Round-trip | write → read → identical values for every channel |
| L2 | Truncated file | detected and reported; recoverable up to the last valid block |
| L3 | Corrupt block | CRC failure; the block is skipped and flagged, not silently misread |
| L4 | Seek performance | random access within the stated bound (target: instant for a 1 GB file) |
| L5 | Old-version file | opens with a version warning; unknown channels ignored, known channels correct |
| L6 | Rate-group accuracy | each group is sampled at its configured rate within tolerance |
| L7 | Throughput ceiling | exceeding it is refused with an explanation, not silently degraded |
| L8 | Map-trace validity | every logged cell matches the cell the ECU reported live for the same instant |
| L9 | Playback determinism | replaying a log reproduces the same dashboard and cell state |
| L10 | Drop accounting | if data was dropped, the file says so |
