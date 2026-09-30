# ECU COMMUNICATION PROTOCOL SPECIFICATION

**Document ID:** PROTO-001 · **Status:** PHASE 1

One protocol, three transports (USB CDC, CAN, optional wireless). The simulator speaks the
*same* protocol as real hardware (spec §85) — an explicit requirement, and the reason the
transport is an interface rather than a global.

---

## 1. Framing

```
 0       1       2       3       4       5       6       7       8
 ┌───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┐
 │ MAGIC │ VER   │ TYPE  │ FLAGS │  SEQ  │    LENGTH     │ TIMESTAMP
 │ 0x52  │ 1 byte│ 1 byte│ 1 byte│ 1 byte│    2 bytes    │  4 bytes
 │  'R'  │       │       │       │       │               │  µs engine time
 ├───────┴───────┴───────┴───────┴───────┴───────┬───────┴───────┤
 │                 PAYLOAD (LENGTH bytes)         │  CRC32C 4 B  │
 └───────────────────────────────────────────────┴───────────────┘
```

| Field | Rule |
|---|---|
| `MAGIC` | `'R'` = 0x52. A frame must begin with this; the resynchroniser hunts for it. |
| `VER` | Protocol version, major/minor packed. A minor bump is backward compatible; a major bump is negotiated. |
| `TYPE` | Message type (below) |
| `FLAGS` | `ACK_REQUIRED`, `IS_RESPONSE`, `IS_FRAGMENT`, `MORE_FRAGMENTS`, `SECURE` |
| `SEQ` | Monotonic per session; out-of-window sequences are rejected (replay protection) |
| `LENGTH` | Payload length, bounded by `MAX_PAYLOAD` per transport (CAN: 8, or 64 for FD) |
| `TIMESTAMP` | **Engine time in µs** derived from the crank, for anything the ECU measured. Never a host receive time — this is what keeps logs valid when the transport stalls. |
| `CRC32C` | Covers VER..PAYLOAD. Integrity only; authenticity comes from the session (§3). |

Framing is byte-oriented and self-synchronising, so a dropped byte cannot permanently
desynchronise the link.

---

## 2. Message types

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `HELLO` | both | version, protocol range, nonce, session id |
| `0x02` | `CAPABILITIES` | ECU→host | capability bitmask + descriptor blob + limits |
| `0x03` | `TELEMETRY` | ECU→host | engine state frame (see `LOG_FORMAT.md`) |
| `0x04` | `TRIGGER_LOG` | ECU→host | tooth log block |
| `0x05` | `EVENT_LOG` | ECU→host | event records |
| `0x06` | `CAL_READ` | host→ECU | id, count |
| `0x07` | `CAL_WRITE_STAGE` | host→ECU | data destined for the inactive slot |
| `0x08` | `CAL_COMMIT` | host→ECU | commit record |
| `0x09` | `CAL_ACTIVATE` | host→ECU | activate a committed slot |
| `0x0A` | `CAL_ROLLBACK` | host→ECU | re-activate the previous slot |
| `0x0B` | `LOG_START` / `LOG_STOP` | host↔ECU | channel set, rate, duration |
| `0x0C` | `LOG_DATA` | ECU→host | block of log frames |
| `0x0D` | `DIAG_READ` | host→ECU | subsystem id |
| `0x0E` | `DTC_READ` / `DTC_CLEAR` | host↔ECU | — |
| `0x0F` | `FREEZE_FRAME` | ECU→host | snapshot captured at fault time |
| `0x10` | `OUTPUT_TEST` | host→ECU | channel, mode, duration — **engineering mode only** |
| `0x11` | `FW_INFO` | ECU→host | version, hash, active slot, signature status |
| `0x12` | `FW_DATA` | host↔ECU | update blocks |
| `0x13` | `FW_COMMIT` | host→ECU | finalise and activate |
| `0x14` | `AUTH_CHALLENGE` / `AUTH_RESPONSE` | both | session authentication |
| `0x15` | `SESSION` | both | role, permissions, lifetime |
| `0x16` | `CAN_DB_UPDATE` | host→ECU | CAN signal database |
| `0x17` | `TRIGGER_CONFIG` | host→ECU | trigger profile + phase |
| `0x18` | `MODE_SET` | host→ECU | mode id (a transaction) |
| `0x19` | `ARM` / `DISARM` | host→ECU | engine-control arming (gated, §98) |
| `0x1A` | `ENGINEERING_MODE` | host→ECU | elevate the session (authenticated) |
| `0x1B` | `TIME_SYNC` | host→ECU | wall-clock metadata only; never used for engine timing |
| `0x1C` | `PROJECT_META` | both | project identity, engine profile, notes |
| `0x1D` | `EOL_TEST` | host↔ECU | factory test control and report |
| `0x1E` | `PING` / `PONG` | both | liveness and round-trip measurement |
| `0x1F` | `ACK` / `NAK` | both | status + error code |

---

## 3. Sessions, roles, security (§60)

```
roles:  VIEWER  <  TUNER  <  ENGINEER  <  FACTORY      (capabilities are additive)
```

Handshake:

```
host → HELLO (version, random nonce, host credential id)
ECU  → HELLO (version, capabilities, ECU identity, challenge)
host → AUTH_CHALLENGE (requested role, signed response)
ECU  → AUTH_RESPONSE (session id, granted role, expiry, audit entry)
```

- **No universal development password.** The default credential set contains no shared
  secret; per-device credentials are provisioned at manufacture. An engineering-mode
  request without valid credentials is refused *and audited*.
- Sessions have a bounded lifetime and a bounded command budget. An expired session cannot
  write calibration.
- Every privileged action is written to a **signed audit log** (who, what, when, outcome)
  which cannot be quietly edited.
- A `SECURE` frame requires at least the TUNER role; an insecure frame for a privileged
  type is dropped and audited.

---

## 4. Fragmentation and flow control

- A payload larger than the transport MTU is fragmented with `IS_FRAGMENT` /
  `MORE_FRAGMENTS` and a fragment index. Reassembly is bounded (maximum fragment count,
  total-size cap) and times out.
- Flow control is a credit scheme; the ECU advertises a receive window.
- **The ECU never blocks the control loop to send** (§104).
- If the host stalls, the engine is unaffected; the session degrades after a bounded time
  and reconnects safely. Losing the laptop must never change engine behaviour.

---

## 5. Firmware update (§59)

```
FW_INFO     → current version, active slot, minimum acceptable version
FW_DATA ×N  → blocks written to the INACTIVE slot, each acknowledged
FW_COMMIT   → verify assembled image: signature → per-block CRC → whole-image hash
              → write commit record → VERIFY → ACTIVATE
```

- Interrupted at any point ⇒ the active slot is untouched and the ECU keeps running.
- An invalid signature is rejected and the attempt is audited.
- Bootloader updates use the same discipline and require the FACTORY role.
- **Never brick**: if the active slot is invalid, the ECU enters recovery (engine control
  disarmed, hardware outputs off) and accepts a new image over USB/CAN.

---

## 6. Transport adapters

| Transport | MTU | Notes |
|---|---|---|
| USB CDC | up to 4096 (bulk) | Preferred for tuning and updates. USB activity cannot perturb the real-time path — a hard architectural requirement. |
| CAN 2.0 | 8 bytes | Heavily fragmented; a small dedicated transport profile. |
| CAN-FD | 64 bytes | Preferred for logs when the profile reports `CAP_CAN_FD`. |
| Wireless (optional) | as USB | A *transport* only. If the radio is absent, the ECU behaves identically. |

Each transport implements the same interface: `open / close / send / on_frame /
set_bitrate`. The simulator, the HIL, and the real ECU are interchangeable behind it.

---

## 7. Error handling

| Error | Behaviour |
|---|---|
| Bad CRC | drop, count, no response |
| Out-of-window sequence | drop, count, possible session reset |
| Unknown type | `NAK ERR_UNKNOWN_TYPE` (forward compatibility) |
| Unauthorised | `NAK ERR_AUTH` + **audit entry**, rate-limited |
| Malformed payload | `NAK ERR_PAYLOAD`; no partial state change |
| Host stall | engine unaffected; session degraded; safe reconnect |

**Rule:** a communication failure never changes engine-control state, except through an
explicit, authenticated, already-validated command.

---

## 8. Conformance tests

| ID | Test | Pass condition |
|---|---|---|
| PR1 | Every message type round-trips on every transport | identical semantics |
| PR2 | Byte corruption at every position | detected by CRC |
| PR3 | Sequence replay | rejected |
| PR4 | Maximal payload fragmentation | reassembles correctly; over-long sequences rejected |
| PR5 | Insufficient role for every privileged type | refused |
| PR6 | Firmware update interrupted at every block boundary | ECU keeps running the old image |
| PR7 | Invalid signature | rejected and audited |
| PR8 | 60 s host stall during active control | engine behaviour and timing unchanged |
| PR9 | Reconnect mid-session | state resynchronises; no partial command applied |
| PR10 | Simulator conformance | the simulator implements the same conformance suite |
