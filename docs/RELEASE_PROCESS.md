# RELEASE PROCESS

**Document ID:** REL-001 · **Status:** PHASE 1

A release is an artefact set with a manifest, built deterministically, from a known commit,
with a known toolchain, with every claim in the documentation traceable to evidence.

---

## 1. What a release contains

```
release/<version>/
  firmware/
    raccoon_<profile>.elf         (with symbols)
    raccoon_<profile>.bin         (the image actually flashed)
    raccoon_<profile>.map         (memory map, for WCET/RAM/flash review)
    raccoon_<profile>.sym
    bootloader.elf / .bin
    calibration/
      default_<engine>.cal        (with signature)
      gt650_baseline.cal
    manifest.json
  tuner/
    raccoon-tuner-<version>       (installer / AppImage / wheel)
    THIRD_PARTY_NOTICES.txt
    LICENSE.txt
  docs/
    (the full documentation set as PDF/Markdown)
  test-reports/
    unit.json  sil.json  static-analysis.json  hil.json  eol-dryrun.json
  hashes.sha256
  CHANGELOG.md
```

---

## 2. `manifest.json`

```json
{
  "product": "RACCOON V2",
  "version": "0.1.0",
  "git_commit": "<full sha>",
  "git_dirty": false,
  "calibration_schema_version": 7,
  "build": {
    "compiler": "arm-none-eabi-gcc 15.2.1",
    "cmake": "3.28.3",
    "flags": ["-O2", "-g", "-ffunction-sections", "..."],
    "toolchain_file_hash": "sha256:..."
  },
  "hardware_profile": "raccoon_v2_rev_a",
  "capabilities": ["CAP_CKP","CAP_CMP","CAP_SEQUENTIAL","CAP_NARROWBAND","CAP_USB", "..."],
  "dependencies": [ { "name":"...", "version":"...", "licence":"..." } ],
  "signing_key_id": "raccoon-release-2026a",
  "built_utc": "2026-11-04T09:12:33Z",
  "source_derived_from": ["git tag v0.1.0"]
}
```

The manifest is signed. `git_dirty: true` **fails the release build** — a release must come
from a clean tree.

---

## 3. Determinism

- The build is run twice from a clean checkout and the artefacts are compared by hash.
  Any difference is a build-reproducibility defect and is investigated.
- No timestamps, paths, or random values are embedded in the image.
- The toolchain is pinned by version and by hash.

---

## 4. Pre-release gates (all must be green)

| Gate | Evidence |
|---|---|
| Clean build, zero warnings | CI log |
| Deterministic rebuild | hash comparison |
| Full unit + property + fuzz | `test-reports/unit.json`, `sil.json` |
| Sanitizers clean | CI log |
| Static analysis clean | `static-analysis.json` |
| Python gates clean | CI log |
| Layer/lint/licence gates clean | CI log |
| Codegen reproducible | CI log |
| Calibration defaults within bounds | build-time assertion |
| HIL report complete with the required measurements | `hil.json` |
| EOL dry run | `eol-dryrun.json` |
| No open CRITICAL/HIGH finding from the adversarial review (§125) | review report |
| `UNKNOWN_REGISTER` reviewed: no item silently closed | diff review |
| Documentation audit: every document exists and is current | audit |
| No dev bypass flags in a release build | build gate |
| No GPL/unknown-licensed dependency | licence scan |
| `SAFETY_CASE` gaps reviewed and each one explicitly accepted or fixed | `SC-001` §6 |

---

## 5. Versioning

| Artefact | Scheme | Meaning |
|---|---|---|
| Firmware | MAJOR.MINOR.PATCH | MAJOR = hardware-incompatible change; MINOR = features; PATCH = fixes |
| Calibration schema | integer, monotonic | Incremented on any layout change; the ECU negotiates it |
| Protocol | MAJOR.MINOR | MAJOR = incompatible; MINOR = additive |
| Log format | integer | Incremented on any incompatible change; old files still open |
| Application | MAJOR.MINOR.PATCH | as firmware |
| Hardware | `rev_a`, `rev_b`, … | Board revision; a profile per revision |

A firmware downgrade below the recorded anti-rollback minimum requires an authenticated
engineer override (`SECURITY_SPEC` §7).

---

## 6. Signing and key custody

| Key | Custody | Notes |
|---|---|---|
| Firmware release key | offline, hardware token, dual control | Never on a build machine |
| Per-device identity key | provisioned at EOL, never exported | Written once |
| Audit-log key | device-local, derived | Not extractable |
| Development/test keys | CI only, clearly named, rejected by release builds | A build that requires them cannot produce a release manifest |

Key rotation is a documented procedure; a compromised release key requires a mandatory
MINOR bump so that devices refuse the revoked range.

---

## 7. Publishing

1. Tag the commit.
2. Build from the tag in a clean container; verify determinism.
3. Sign the manifest; hash every artefact.
4. Run the licence/notice bundler to produce `THIRD_PARTY_NOTICES.txt`.
5. Publish artefacts + docs + test reports.
6. Publish the CHANGELOG with **measured** changes and an explicit statement of what is
   *not* yet verified.
7. Tag the calibration baselines.

---

## 8. Documentation audit

Before release, verify that every document in the spec's list exists, has a status, and
contains no absolute claims:

```
ARCHITECTURE · ENGINE_CONTROL_SPEC · TRIGGER_SPEC · FUEL_SPEC · IGNITION_SPEC
TORQUE_CONTROL_SPEC · SENSOR_SPEC · ACTUATOR_SPEC · CAN_SPEC · CALIBRATION_SPEC
LOG_FORMAT · PROTOCOL_SPEC · GUI_ARCHITECTURE · AUTOTUNE_SPEC · DYNO_SPEC
HIL_SPEC · TEST_PLAN · SAFETY_CASE · REFERENCE_ECU_RESEARCH · LICENSE_REGISTER
UNKNOWN_REGISTER · RELEASE_PROCESS
```

plus this project's additions: `PRODUCT_REQUIREMENTS`, `MCU_SELECTION`, `IO_MATRIX`,
`FIRMWARE_ARCHITECTURE`, `TESTING_ARCHITECTURE`, `SECURITY_SPEC`, `GT650_RESEARCH`.

**Claim audit:** every performance, timing, and safety statement in the documentation must
carry a verification label. A claim without one is removed or labelled `UNKNOWN`.

---

## 9. Post-release

| Item | Cadence |
|---|---|
| Field feedback on faults/DTCs | continuous |
| `UNKNOWN_REGISTER` update | continuous |
| Dependency updates | monthly, with a full gate run |
| Security review | before any release touching auth/keys |
| Adversarial review | every release |
| Endurance re-validation | before any release that touches the real-time region |

---

## 10. Status

No release exists. This document defines the process that the first release will follow.
