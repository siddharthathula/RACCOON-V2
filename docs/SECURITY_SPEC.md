# SECURITY SPECIFICATION

**Document ID:** SEC-001 · **Status:** PHASE 1

Scope: the ECU's protection against unauthorised modification, and the guarantees required
by spec §59, §60, §106. This is **not** a security-certification effort; it is the set of
mechanisms a professional ECU must have so that a tune or a firmware image cannot be
altered by accident or by a well-meaning mistake.

---

## 1. Trust model

| Party | Trusted with | Not trusted with |
|---|---|---|
| Owner (dealer/tuner) | Calibration within a role; read telemetry; view diagnostics | Raising safety limits beyond a compiled ceiling; firmware signing keys |
| Engineer | Safety-limit changes (**lowering only**), engineering-mode features | Root signing key; production key |
| Factory | Firmware signing, key provisioning, EOL configuration | — |
| Anyone with physical access to the USB/CAN port | Nothing by default. A session must be established and authenticated | Any write, until authenticated |

**Default posture:** a freshly powered ECU, with no session, exposes *read-only* telemetry
and refuses every write. Engine control is disarmed. This is the safe default and it is
what the state after power-on looks like until a valid tune and arm request are accepted.

---

## 2. Identity

| Item | Mechanism |
|---|---|
| Unique device identity | Derived from the MCU's unique ID plus a provisioned serial, combined into a device key pair stored in secured flash |
| Serial number | Printed on the enclosure, programmed at EOL, bound to the calibration images |
| Key storage | Write-once / secured memory region where the MCU provides it; otherwise a protected flash region with the debug port disabled in production |
| No shared secrets | There is **no** universal development password. Dev credentials exist only in development builds, and a release build **fails to compile** if they are enabled |

---

## 3. Roles and permissions

```
VIEWER   read telemetry, logs, diagnostics
TUNER    + read/write calibration (non-safety), mode selection, logging control,
          map edits, and (with an explicit confirmation) calibration commits
ENGINEER + engineering mode: lower hard limits, output tests, trigger override,
          calibration rollback/branch/merge, safety-limit engineering
FACTORY  + firmware signing, key provisioning, EOL, key rotation
```

Enforcement points:
- The **ECU** enforces permissions, not the GUI. A crafted message from a `VIEWER`
  session cannot write.
- Every message type declares a minimum role in the schema; the check is in one place.

---

## 4. Session protocol

```
1. host → HELLO (protocol version, nonce, credential id, requested role)
2. ECU  → HELLO (version, capabilities, device identity, challenge, policy)
3. host → AUTH_CHALLENGE (signed response over the challenge, using a device/user key)
4. ECU  → AUTH_RESPONSE (session id, granted role, expiry, audit entry)
5. every privileged frame carries the session id and a sequence
```

- **Bounded session lifetime** and a **bounded command budget**; an expired session cannot
  write.
- Session loss does not disturb the running engine.
- Repeated authorisation failures are rate-limited and audited.

*(The exact signature algorithm and key provisioning method are `UNK-SW-003`, pending a
security design review. The requirement here is that they are asymmetric, implemented in
hardware-accelerated crypto where available, and reviewable.)*

---

## 5. Firmware integrity

| Property | Mechanism |
|---|---|
| Authenticity | Signed image; the bootloader verifies the signature before jumping |
| Integrity | Per-block CRC + whole-image hash |
| Atomicity | A/B slots with a commit record; the active slot is untouched until commit |
| Recoverability | If no valid slot exists, recovery mode: engine control disarmed, outputs off, update path available over USB/CAN |
| Version policy | Refuse an image older than the recorded minimum; refuse a version mismatch without an explicit, authenticated override |
| Bootloader protection | The bootloader itself is updatable only by FACTORY, with the same A/B discipline |
| Debug | JTAG/SWD disabled ( RDP level 2 or equivalent, where the MCU provides it) in production builds; the release build asserts it |

---

## 6. Calibration integrity

- Calibration images are CRC-protected per block and signature-protected as a whole.
- A write is only accepted from an authenticated `TUNER`+ session and only through the
  transaction sequence in `CALIBRATION_SPEC.md`.
- `SAFETY`-class items may only be **lowered**; a request to raise one above its compiled
  ceiling is refused by the ECU regardless of role, including `ENGINEER`.
- Every privileged action is written to a **signed audit log** (who, what, when, outcome,
  and a monotonic counter) that survives power cycles.

---

## 7. Anti-rollback and version handling

- The ECU records the highest firmware version it has accepted.
- An older version is refused unless an authenticated `ENGINEER` explicitly overrides it,
  and the override is audited.
- Calibration schema versions are negotiated; a mismatch causes a defined
  upgrade/downgrade/refuse decision, never a misinterpretation.

---

## 8. Physical and debug protections

| Protection | Status |
|---|---|
| Debug port disabled in production | Required; verified at EOL |
| Secured key storage | Required where the MCU supports it |
| Tamper evidence | Enclosure/case design — noted, not a software feature |
| External connector exposure | USB/CAN are exposed by necessity; therefore unauthenticated access is read-only and rate-limited |

---

## 9. What this design does not claim

- It is not a defence against an attacker with physical possession and the ability to
  replace the ECU.
- It is not a security certification, and it is not ASIL-aligned.
- It does not hide the existence of a calibration schema; an attacker who can read the
  calibration can learn the structure. Confidentiality of the tune is **not** a goal —
  integrity and authenticity are.

---

## 10. Security tests

| ID | Test | Pass condition |
|---|---|---|
| SEC-1 | Unauthenticated session | no write is accepted; every attempt is audited |
| SEC-2 | `VIEWER` writing a safety item | refused |
| SEC-3 | `TUNER` raising a hard limit above the compiled ceiling | refused, regardless of role |
| SEC-4 | `ENGINEER` lowering a hard limit | accepted and audited |
| SEC-5 | Forged signature on a firmware image | rejected; audit entry |
| SEC-6 | Corrupted calibration image | rejected; previous image runs |
| SEC-7 | Rollback attempt to an old firmware | refused without an authenticated override |
| SEC-8 | Debug port in a production build | disabled; verified at EOL |
| SEC-9 | Development credentials in a release build | build fails |
| SEC-10 | Session expiry | writes refused; reads still allowed; engine unaffected |
| SEC-11 | Audit log tampering | the signature check fails and the tampering is detected |
| SEC-12 | Replay of a privileged frame | rejected (sequence + session binding) |
