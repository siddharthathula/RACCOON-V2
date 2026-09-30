# THIRD-PARTY LICENSE REGISTER (CLEAN-ROOM RECORD)

**Document ID:** TPLR-001
**Status:** PHASE 0 — active for the whole project
**Owner:** RACCOON V2 engineering
**Rule:** A row here is required **before** any third-party material is read into the
build, vendored, linked, or translated. Reading public documentation is permitted;
copying is not, unless a row explicitly says so and the licence permits it.

---

## 1. Register

`SOURCE USED` = what kind of artefact was actually read.
`CODE COPIED?` = the only question that creates a copyleft obligation.

| # | Project | License (verified) | Source used | Information used | Code copied? | Dependency? | Legal implication |
|---|---|---|---|---|---|---|---|
| TPL-001 | Speeduino | GNU GPL **v2.0** (GitHub repo metadata, verified 2026-09-29) | Public wiki, public doxygen, public PDF manual, public GitHub landing page | Trigger-decoder taxonomy, configurable trigger concepts (angle, edge, skip revolutions, filter), injector high-Z/low-Z concepts, calibration UI expectations | **NO** | No | GPL-2.0 is strong copyleft. Any copying of *code* would oblige us to publish derivative firmware under GPL-2.0. We copy **no** Speeduino code, headers, structure translations, or table layouts. Ideas and terminology (generic to the field) are not copyrightable expression and are used independently. |
| TPL-002 | rusEFI | **GNU GPL v3 + 3 additional terms** (rusEFI#1 off-road land/water only, rusEFI#2 not automotive-emissions compliant, rusEFI#3 not for manned aircraft) — verified 2026-09-29 at `github.com/rusefi/rusefi/blob/master/license.txt` | Public website, public HTML architecture docs, public wiki | Run-once-per-revolution scheduling model, hybrid angle→time offset scheduling, timer-budget discipline, "compute then schedule from nearest trigger event" principle, feature landscape | **NO** | No | GPL-3.0-family with extra restrictions ⇒ incompatible with a closed commercial product. We reimplement the *principles* from public prose. **Engineering note:** the additional terms mean even a GPL release of RACCOON firmware could not satisfy rusEFI's own use restrictions, so there is no licensing path that makes reuse safe. Clean-room is the only option. |
| TPL-003 | TunerStudio MS | Proprietary commercial. Licence text verified 2026-09-29 (`efianalytics.com/register/license.jsp`). Prohibits modification, reverse compilation, translation, disassembly, repackaging; commercial redistribution requires separate agreement | Public licence text, public product pages | Calibration workflow and UX vocabulary; existence of VE-analyse-style statistical correction; fact that UI is ini-described | **NO** | No — and must never link or bundle | Using, bundling, or reverse engineering TunerStudio is prohibited by its own licence. RACCOON is an **independent** application. We do not ship, import, or emulate TunerStudio binaries or its generated ini artefacts. |
| TPL-004 | MegaLogViewer / MegaLogViewer HD | Proprietary commercial, same licensor as TPL-003 | Public product pages | Data-analysis workflow expectations (offline log analysis as a first-class product) | **NO** | No | Same prohibition as TPL-003. |
| TPL-005 | MegaSquirt / MS3 family | GPL-family (open source ecosystem) | Community/public documentation only | Ini/serial/log workflow expectations; trigger compatibility expectations | **NO** | No | Treated as GPL. No code reuse. |
| TPL-006 | Haltech (Elite, Nexus) | Proprietary. Haltech NSP closed | Public vendor comparison tables | Capability landscape, product-tiering pattern, capability gating pattern | **NO** | No | Marketing/comparison data used for market analysis only. No code, no protocol, no UI asset. |
| TPL-007 | Link ECU (G4X/G5) + Link PDM | Proprietary. Closed | Public/dealer comparison charts | Capability landscape; trigger oscilloscope as a product feature; uniform logging floor | **NO** | No | As TPL-006. |
| TPL-008 | FuelTech PowerFT | Proprietary. Closed | Public/dealer comparison charts | Capability landscape; drag-mode feature set; integrated dash pattern | **NO** | No | As TPL-006. |
| TPL-009 | MaxxECU | Proprietary. Closed | Public vendor comparison table | Capability landscape; entry-level 4-sequential-cylinder product evidence | **NO** | No | As TPL-006. |
| TPL-010 | ECUMaster | Proprietary. Closed | Public marketing only | Segment confirmation (professional class) | **NO** | No | As TPL-006. |
| TPL-011 | MoTeC (M1 ECU, i2 software) | Proprietary. Closed | Public product material only | Professional-tuner workflow expectations; session-based data management | **NO** | No | As TPL-006. |
| TPL-012 | Syvecs | Proprietary. Closed | Public product material only | Hardware-abstraction product-family pattern | **NO** | No | As TPL-006. |
| TPL-013 | Bosch Motorsport (MS ECU family) | Proprietary. Closed | Public product pages for Mini-LSU 4.9 | That CJ125 is the required mated controller IC for LSU 4.9/4.2; published λ=0.65→air monotonic range; published amplification factors (v=17 lean, v=8 rich) | **NO** | Possible future *component* purchase (see TPL-020) | Datasheet-level facts about a **purchasable component** are used to design our own front end. No Bosch firmware, code, or reference design is copied. Bosch reference designs are copyrighted; we write our own schematic. |
| TPL-014 | Marelli Motorsport | Proprietary. Closed | Public product material only | Segment confirmation | **NO** | No | As TPL-006. |
| TPL-015 | Bosch CJ125 (wideband controller IC) | Proprietary silicon, sold as a component | Public part descriptions | Interface expectations for driving LSU 4.9 | **NO** | **Yes — planned.** Purchasing the IC is a component choice, not a code dependency | Component purchase carries Bosch supply/availability risk → `UNK-HW-004`. We will not ship a native-wideband capability without it physically present. |
| TPL-016 | MAX9924/9926/9927 (Analog Devices, VR sensor interface) | Proprietary silicon; **datasheet is publicly redistributable** | Public datasheet (Rev.5, 2019-03-28) | VR conditioning architecture: differential input stage, adaptive peak threshold ≈ 1/3 of previous cycle peak, zero-crossing output, 85 ms watchdog threshold recovery, 5 Hz–25 kHz operating range, −40 °C to +125 °C, 10 kΩ series input resistors, input filter capacitor | **NO** | **Yes — planned component** | Datasheet-derived circuit design is normal engineering practice for a purchased part. We write our own schematic/layout. The "≈1/3 previous peak" behaviour is a *functional* fact we design our trigger-filter policy *around* (it explains the amplitude growth pattern Speeduino's aggressive filter is used to reject). |
| TPL-017 | NXP MC33810 / MC33813 (smart driver: injector + ignition pre-driver) | Proprietary silicon; public datasheets | Public datasheets | Smart-injector peak-and-hold with current limit, ignition IGBT gate pre-driver with dwell current sense, spark-duration (SPKDUR) feedback, max-dwell protection, soft shutdown, FBx divider 9:1 recommendation, 4.5–36 V VPWR | **NO** | **Yes — candidate component** | The FBx spark-duration feedback path is architecturally important: it turns "we commanded a spark" into "a spark voltage event was observed", which is exactly the measurement RACCOON's safety case needs. Component choice is `UNKNOWN` until the hardware phase decides between a discrete solution and a smart-driver IC. |
| TPL-018 | STM32 H7/G4 families + HAL/CMSIS | Apache-2.0 / BSD-3-Clause (ST) | Public reference manuals, HAL source | MCU peripheral semantics, timer/DMA/ADC/FDCAN behaviour | **NO** — but a **runtime dependency** | **Yes** | Apache-2.0/BSD-3 permissive. Notice files must be preserved in any distributed binary. We will depend on HAL only in `firmware/hardware/`, never in the portable engine core. |
| TPL-019 | Python ecosystem (PyQt6, NumPy, SciPy, pyqtgraph, pytest, ruff, mypy, bandit, pydantic/dataclasses) | LGPL-3.0 (PyQt6), BSD-3 (NumPy/SciPy/pyqtgraph), MIT (pytest/ruff/mypy/pydantic) | Public docs and package metadata | Library selection | **NO** | **Yes** | PyQt6 LGPL-3.0: dynamic linking only, no static linking, and we must not modify it in a way that forces relicensing. Distribution must include LGPL notices and offer relinking. See `RELEASE_PROCESS.md` §compliance. |
| TPL-020 | ARM Cortex-M7/M4 core + CMSIS headers | Apache-2.0 (Cortex-M) | Public ARM docs | Core behaviour | **NO** | **Yes** | Permissive. Notices preserved. |
| TPL-021 | Any AI/LLM model used inside the shipped product | varies | n/a | n/a | **NO** | **No — see note** | The AI tuning assistant (§84/§115) is **advisory only and runs on the PC**. No model weights ship in the ECU. If a model is ever embedded, its licence must be registered here first and its weights must be distributable. |

---

## 2. Clean-room protocol (mandatory)

1. **Read public documentation freely.** Architecture, features, terminology, and
   published specifications are facts about the world.
2. **Never open a GPL/CE source file while implementing a RACCOON module.** If a
   contributor must consult one, that module is re-implemented by a different
   contributor from the written design, and the design note records the consultation.
3. **Every RACCOON module carries a `DESIGN-NOTE` header** listing: the requirement IDs it
   satisfies, the public sources consulted (URLs, dates), and an explicit
   `INDEPENDENTLY AUTHORED` statement.
4. **No file in this repository may be a transcription, a translation, or a
   mechanically-derived work of a GPL file**, regardless of how small.
5. **No GPL-licensed runtime dependency in the ECU firmware or the tuner application.**
   If a feature requires one, it must be an optional out-of-process tool with a documented
   licence boundary (currently: none).
6. **A violation invalidates the affected module** and requires re-authoring, not
   patching. This is a release blocker (`RELEASE_PROCESS.md`).

---

## 3. What is *not* copyrightable here (used freely)

- The physical idea of conditioning a VR sensor and counting zero crossings.
- The 720° four-stroke cycle, firing orders, TDC definitions, and volumetric equations.
- Trigger-pattern geometry (36-1, 60-2, …) — these are *numbers* describing wheels.
- Standard ECU concepts: closed loop, dead time, dwell, VE table, launch control.
- Published device datasheet *functional* parameters and recommended application values
  (for purchased components), while the reference *schematic* remains protected.

---

## 4. Compliance obligations for the shipped product

| Obligation | Applies to | Where satisfied |
|---|---|---|
| Preserve copyright + licence notices in binaries/distributions | ST HAL/CMSIS, ARM CMSIS, all Python deps | `release/` manifest + `tools/packaging` notice bundler; `RELEASE_PROCESS.md` §3 |
| LGPL-3.0 dynamic-linking compliance for PyQt6 | Tuner app | Packaging config; no PyQt6 modification; source-offer/replaceability path documented in user docs |
| Apache-2.0 attribution | ST/CMSIS in firmware | `THIRD_PARTY_NOTICES.txt` emitted at build time by `tools/packaging` |
| No GPL code in the shipped product | Everything | This register + `tools/analysis/license_scan.py` gate in CI (to be implemented, `TASK` in `TEST_PLAN.md` §static-analysis) |
| Component purchase terms (Bosch CJ125, ADI MAX9926, NXP MC33810) | Hardware BOM | `hardware_profiles/` + BOM with datasheet revision recorded per part |
