# REFERENCE ECU RESEARCH

**Document ID:** RCR-001
**Status:** PHASE 0 COMPLETE (public-information research)
**Compiled:** 2026-09-29
**Method:** public vendor documentation, public wikis, public doxygen, public datasheets, GitHub repository metadata. No source code was copied, decompiled, or reverse engineered.
**Boundary note:** This document records *architecture and feature ideas observed from public material*. Where a number is quoted it is quoted **as published by the vendor/project** and is **not** an engineering input to RACCOON calibration. Calibration inputs are handled in [`UNKNOWN_REGISTER.md`](UNKNOWN_REGISTER.md).

---

## 0. READING THIS DOCUMENT

Each system is analysed with:

```
SYSTEM / SOURCE / LICENSE / FEATURE / ARCHITECTURAL IDEA / ADVANTAGE / LIMITATION
/ RACCOON IMPLEMENTATION IDEA / RACCOON DIFFERENTIATOR
```

Verification status of every claim in this document is one of:

| Label | Meaning |
|---|---|
| `VERIFIED-PUB` | Read directly from the cited public source on the compilation date |
| `VERIFIED-PUB-2ND` | Read from a second independent public source |
| `INFERRED` | Reasonable engineering inference, not stated by the source |
| `UNKNOWN` | Not established; tracked in `UNKNOWN_REGISTER.md` |

---

## 1. Speeduino

**SYSTEM:** Speeduino — open-source stand-alone EMS for hobbyist/enthusiast engines.
**SOURCE:** <https://github.com/speeduino/speeduino> (GPL-2.0 per repository metadata, `VERIFIED-PUB` 2026-09-29); <https://wiki.speeduino.com/decoders>; <https://speeduino.github.io/speeduino-doxygen/group__dec__uni.html>; <https://speeduino.com/Speeduino_manual.pdf>
**LICENSE:** GNU GPL v2.0 (firmware). `VERIFIED-PUB`

| FEATURE | Evidence | Status |
|---|---|---|
| MCU class | Arduino Mega/Teensy/STM32F4-class boards | `VERIFIED-PUB` |
| Injector drives | 4 injector circuits, up to 8 injectors/cylinders; high-Z native, low-Z via series resistors | `VERIFIED-PUB` |
| Ignition | Logic-level / dumb-coil + external driver | `VERIFIED-PUB` |
| Trigger | Long documented decoder list: missing tooth, missing tooth (cam), dual wheel, non-360, etc. | `VERIFIED-PUB` |
| Trigger conditioning | **Firmware requires signals already conditioned to a 3.3–5 V square wave** | `VERIFIED-PUB` |
| Crank sync controls | Trigger angle (ATDC of tooth #1), trigger edge, skip revolutions, trigger filter level, re-sync every cycle | `VERIFIED-PUB` |
| Cam handling | Secondary trigger pattern, "poll level" cam decoder with level-at-tooth-#1 | `VERIFIED-PUB` |
| Logging | Composite + tooth loggers, real-time & offline | `VERIFIED-PUB` |
| Calibration UI | TunerStudio (third party) | `VERIFIED-PUB` |
| Race features | Not a primary design axis | `INFERRED` |

**ARCHITECTURAL IDEA:** A *decoder abstraction with shared per-decoder capability flags*
(`decoder_features_t`) and a *shared universal decoder layer* that implements
filtering, tooth logging, and edge policy once for all patterns. Configuration is
driven from TunerStudio ini definitions, and the firmware exposes the same structure
the UI edits.

**ADVANTAGE:** Lowest cost of entry; enormous community; trigger setup is unusually well
documented for a DIY product; the "trigger angle + skip revolutions" user model is
genuinely good for first-time installers.

**LIMITATION:**
- The trigger is electrically fragile by design — the firmware explicitly requires conditioned logic-level square waves. VR conditioning is a separate board. There is no protective input architecture in firmware because there is no protective input architecture in hardware.
- Arduino-centric timing model (16 MHz-class micros) limits the resolution/headroom of crank-angle scheduling at high RPM.
- Feature set is broad but shallow relative to motorsport ECUs: no torque coordination layer, no first-class per-cylinder lambda, no launch/traction/flat-shift suite, no DBW.
- GPL-2.0 ⇒ not a source of reusable code for a proprietary product.

**RACCOON IMPLEMENTATION IDEA:**
1. Adopt the *decoder capability-flag + shared layer* idea, but with a first-class
   `sync_manager` state machine (10 states) that Speeduino does not have.
2. Adopt the "trigger angle after TDC" user concept because it is intuitive, but pair it
   with a **TDC verification tool** (see Differentiator 4).
3. Keep conditioning *inside* the product as a defined hardware block with a documented
   signal contract between conditioner and firmware.

**RACCOON DIFFERENTIATOR:** Sync is a *state machine with a confidence scalar and a
recoverable degradation policy*, not a boolean. CRANK/CMP raw tooth timing is a
first-class, viewable signal in the desktop app (Link has a "trigger oscilloscope";
Speeduino has a tooth logger — RACCOON must combine both, with an explicit live
"which decoder is active and why" panel).

---

## 2. rusEFI

**SYSTEM:** rusEFI — open-source STM32-based EMS, RTOS, wide feature set.
**SOURCE:** <https://rusefi.com/>; <https://rusefi.com/docs/html/>; <https://wiki.rusefi.com/>; <https://github.com/rusefi/rusefi> (repo license shown as "Other"); <https://github.com/rusefi/rusefi/blob/master/license.txt>
**LICENSE:** **GNU GPL v3 plus three additional terms** — rusEFI#1 (intended for off-road land and water vehicles), rusEFI#2 (not compliant with automotive environmental regulations), rusEFI#3 (not approved for manned aircraft). `VERIFIED-PUB` 2026-09-29. This is *not* stock GPL-3.0 and is a GPL derivative — treated as GPL-family, copyleft, incompatible with a closed-source product.

| FEATURE | Evidence | Status |
|---|---|---|
| MCU class | STM32, high-performance FPU MCUs, RTOS (per project docs) | `VERIFIED-PUB` |
| Sequential injection | up to 12 cylinders; batch; staged; direct injection | `VERIFIED-PUB` |
| Sequential ignition | up to 12 cylinders; wasted spark; multi-spark | `VERIFIED-PUB` |
| Triggers | "20+ OEM triggers supported" | `VERIFIED-PUB` |
| Fuel | closed loop, wall-wetting/fuel-trip/tau acceleration enrichment, flex fuel, small-pulse correction, low-Z smart injector control | `VERIFIED-PUB` |
| Lambda | onboard wideband controller; dual AFR signal logging | `VERIFIED-PUB` |
| Performer features | DBW, launch, flat shift, antilag, boost control, traction control, knock sensing & response, kick start, rotary engines, automatic transmission control, Lua scripting, blending, Bluetooth, digital dash | `VERIFIED-PUB` |
| Boost-by-gear | listed in one table as unsupported / in development | `VERIFIED-PUB` (status inconsistent across its own pages) |
| Hardware | `uaEFI`, `microRusEFI`, plus a high-feature board: 180 MHz Cortex-M7, 2× VR inputs, 6× hall/digital, 16×4 A low-side, 12× ignition outputs, dual H-bridges, 12 analog + 4 temp inputs, 4× high-side, internal logging | `VERIFIED-PUB` |
| Simulator | PC simulator of firmware | `VERIFIED-PUB` |
| Scheduling model | Hybrid angle+time: schedule coil charge as "N degrees after event X at current RPM", spark as "N degrees after event Y" | `VERIFIED-PUB` |
| Timer budget | documented 5 timers: 1 MHz microsecond timer, 10 kHz fast ADC, slow ADC, periodic fast, periodic slow | `VERIFIED-PUB` |
| Config plumbing | `rusefi_config.txt` is the single source of persistent configuration, generated and size-locked against the TunerStudio ini (`TS_FILE_VERSION`) | `VERIFIED-PUB` |
| Tuning UI | TunerStudio; rusEFI console (own Java tool) | `VERIFIED-PUB` |

**ARCHITECTURAL IDEA (the single most valuable idea in the open-source landscape):**
> "rusEFI runs on crank/cam trigger events. Once per crank revolution we evaluate the
> amount of needed fuel and the spark timing. Once we have decided on the parameters for
> this revolution we **schedule all the actions to be triggered by the closest trigger
> event**."
> — <https://rusefi.com/docs/html/>

Plus the **hybrid angle/time offset** insight: the position sensor gives you a *set of
events at known angles*, not a continuous position; therefore scheduling is expressed as
*"X degrees after event E"*, converted to microseconds at the currently measured period.
This is the correct model, and RACCOON adopts and formalises it (§`docs/ENGINE_CONTROL_SPEC.md`).

**ADVANTAGE:** The broadest documented open feature set; real hardware variety; a
config-as-single-source-of-text-file discipline; a simulator of the actual firmware; a
documented timer budget; multi-cylinder scale.

**LIMITATION:**
- GPLv3 + additional restrictive terms ⇒ **no code reuse** in a proprietary product.
- Config generated from text + hard-coded page size (`PAGE_0_SIZE`) means every
  structural change is a coordinated, error-prone, multi-tool edit.
- Java tooling in the build/data path is heavy for a Python/PyQt6 product.
- Lua scripting and Bluetooth in the same MCU that runs injection scheduling increases
  the "real-time depends on non-deterministic work" risk that RACCOON must not take.
- Multi-cylinder automotive focus (up to 12); no purpose-built 1–4 motorcycle electrical
  or physical design.

**RACCOON IMPLEMENTATION IDEA:**
- Adopt: run-once-per-revolution compute + schedule-from-nearest-trigger-event.
- Adopt: hybrid angle→time offsets, but add **acceleration-aware time prediction** (§`TRIGGER_SPEC.md`).
- **Reject:** text-file-generated config with hard-coded page sizes. Instead: versioned,
  typed, self-describing calibration schema with a stable **ID-addressed** layout so the
  GUI and firmware can evolve independently.
- **Reject:** executing scripts/UI work in the real-time MCU.

**RACCOON DIFFERENTIATOR:** Determinism-by-architecture: the engine-control core is a
strictly bounded, allocation-free, priority-ordered execution region with *no*
dynamic memory, no blocking calls, and no dependency on any communication subsystem.
RACCOON's real-time rule (§104) forbids the very couplings rusEFI tolerates.

---

## 3. TunerStudio / MegaLogViewer (EFI Analytics)

**SYSTEM:** TunerStudio + MegaLogViewer — the de-facto calibration and data-analysis workflow.
**SOURCE:** <https://www.efianalytics.com/register/license.jsp?appName=TunerStudio+MS> (licence text); <https://tunerstudio.com/index.php/downloads>; <https://tunerstudio.com/index.php/products/tuner-studio/tunerstudioms>
**LICENSE:** **Proprietary, commercial, closed source.** Licence explicitly prohibits modification, reverse compilation/translation/disassembly, and repackaging; distribution with commercial products requires a separate agreement. `VERIFIED-PUB` 2026-09-29. **RACCOON must not link against, embed, or redistribute it.** Protocol compatibility (MS2/MS3 style serial framing, ini-driven UI definition) is a *published wire-format fact* and may be independently reimplemented; the *code and the ini-art generation pipeline are not usable*.

| FEATURE | Evidence | Status |
|---|---|---|
| Role | Live tuning, dashboard, logging, offline analysis | `VERIFIED-PUB` |
| VE Analyse | "filter out junk records, consider all quality data, weighted statistical averages" | `VERIFIED-PUB` (vendor marketing text) |
| VE Analyse Live | live, in-car VE correction with dynamic offset record seeking | `VERIFIED-PUB` (vendor marketing text) |
| Offline tooling | MegaLogViewer, HD edition | `VERIFIED-PUB` |
| Platforms | Windows, Mac, Linux | `VERIFIED-PUB` |
| UI definition model | ini-file-driven form/dash definition, generated from firmware config | `VERIFIED-PUB` (from rusEFI integration docs) |

**ARCHITECTURAL IDEA:** (1) *UI described by data, not code* — a declarative form/indicator
definition so one build serves many hardware variants. (2) *The analyser, not just the
recorder, is the product* — offline statistical correction of a live log is the core
tuner loop. (3) Single-binary live + offline workflow.

**ADVANTAGE:** The industry's calibration muscle memory. Everything a tuner already knows
(sliders, veTable, the ini layout, the log format) lives here.

**LIMITATION:** Proprietary, per-seat licensing, Java heritage, dated UI; no 1–4 cylinder
motorcycle packaging; ini generation pipeline requires shipping a Java toolchain.

**RACCOON IMPLEMENTATION IDEA:** Adopt the *declarative UI* idea but implement it natively
in PyQt6 from a Python-typed schema (no codegen, no Java). Adopt the *analyser-as-product*
idea and make it the centre of RACCOON's autotune. Do **not** adopt the .ini format as our
storage format (legal + technical debt); adopt only a *reader* for legacy MS logs if ever
needed, and mark it optional.

**RACCOON DIFFERENTIATOR:** Confidence-aware autotune (§73) — every suggested cell change
carries sample count, variance, gate-rejection history, and an explicit confidence scalar,
and can be applied as a *proposed diff* the tuner accepts, not an automatic write.

---

## 4. MegaSquirt / MS3 family

**SYSTEM:** MegaSquirt 1–3 — the archetype open EFI ECU.
**SOURCE:** public manual/community documentation (no source inspected).
**LICENSE:** GPL-family (project is open source). Treated as **no code reuse**.
**Status of details below:** `INFERRED` / community-documented — not re-verified against a
primary source during this phase. Logged as `UNK-REF-001` in the unknown register.

| FEATURE | Status |
|---|---|
| Sequential/semi-sequential/batch injection | `INFERRED` (widely documented) |
| Speeduino-compatible trigger approach; famous missing-tooth support | `INFERRED` |
| Trigger scope / composite log + TunerStudio workflow | `INFERRED` |
| Limitation | Multi-cylinder automotive heritage, no motorsport performance suite |

**RACCOON IMPLEMENTATION IDEA:** None beyond confirming that the "MS-style ini + log"
workflow is the compatibility target our *import* path should accept.

---

## 5. Haltech

**SYSTEM:** Haltech Elite + Nexus families.
**SOURCE:** <https://www.haltech.com/ecu-buyers-guide/> (comparison tables), <https://www.haltech.com/news-events/new-release-ntk-o2-wideband-kits/>
**LICENSE:** Proprietary firmware + proprietary tuning software (Haltech NSP). Closed.

Published comparison data (`VERIFIED-PUB` 2026-09-29, vendor self-published):

| Capability | Range across the range | Notable |
|---|---|---|
| Peak & hold injector drives | 4 → 18 | Elite 550: 4 … Nexus R5: 18 |
| Ignition drives | 4 → 12 | |
| Analog voltage inputs | 1 → 23 | Nexus R5: 23 analog in |
| Sync pulsed inputs | 0 → 10 | |
| Knock inputs | 0 → 2 | dual on Nexus |
| Onboard wideband | none → 2 × LSU 4.9 | |
| DBW | no → dual-channel | |
| Flex fuel | YES across range | |
| Closed-loop O2 | dual across range | |
| Long-term learning | up to 3D → 4D | |
| VVT | 1 → 4 channels | |
| Data logging | 2 MB → 512 MB | |
| Traction control | no → yes | |
| Launch control | yes across range | |
| Antiblag / rolling antilag | no → yes | |
| Boost control | 3D closed loop → 4D closed loop | |
| Flat shift | basic → advanced | |
| Torque management | no → yes | |
| Internal MAP | 4 bar (external on R5) | |

**ARCHITECTURAL IDEA:** A *tiered hardware family* where a single software platform is
gated by a hardware capability set — exactly the pattern RACCOON must adopt as the
**universal capability model** (§94), but taken further: RACCOON defines the capability
model in *firmware-reported, signed* form so the GUI adapts to whatever board is fitted.

**ADVANTAGE:** Broadest published feature-per-rupee for the enthusiast market; flexible
fuel and long-term learning standard; 4D learning is a real differentiator for
multi-fuel motorcycle use (India-relevant: E20/E85 availability).

**LIMITATION:** 2 MB log on the flagship (vs Link's 512 MB across the range) — a
first-order gap for motorsport data work. Torque management absent on several mid models.
Proprietary ecosystem, no source, no protocol public.

**RACCOON IMPLEMENTATION IDEA:**
- Capability model → adopted (with extension: the capability set is a *bitmask plus a
  descriptor blob* and is included in the signed firmware manifest).
- 4D long-term learning → adopt as an *optional* learner with strict gating (see
  `FUEL_SPEC` learner gating rules) and full observability.
- Log capacity → RACCOON V2 target 512 MB–8 GB of **internal** logging (see `MCU_SELECTION.md`
  external storage section), because §52/§91 require measured logging throughput.

**RACCOON DIFFERENTIATOR:** Logging that is *first-class in the ECU*, not a PC-side
download; plus a trigger oscilloscope and a live calibration trace, so a fault can be
diagnosed from the ECU alone when a laptop is not present.

---

## 6. Link ECU (G4X / G5)

**SYSTEM:** Link G4X AtomX → XtremeX/FuryX, G5 Voodoo.
**SOURCE:** <https://www.impulse-performance.com/pages/link-ecu-comparison>; <https://www.ecuwiring.co.nz/blogs/news/how-to-choose-the-right-ecu-for-your-car-nz-buyers-guide>
**LICENSE:** Proprietary. Closed.

Published comparison data (`VERIFIED-PUB` 2026-09-29, dealer-published):

| Capability | G4X range | Note |
|---|---|---|
| Onboard logging memory | **512 MB on every model** | 250 channels, 100 kHz total rate |
| Injector drives | 4 → 12 | 12 on Voodoo Pro (plus 4 multi) |
| Ignition drives | 4 → 8 | |
| CAN buses | 2 (multiple on G5) | |
| Onboard knock | StormX and up, dual sensor | |
| E-throttle | XtremeX and up (single/dual) | |
| Onboard wideband | FuryX / G5 | |
| Traction control | XtremeX and up | |
| Launch / antilag / gear shift | MonsoonX and up | full suite on Voodoo |
| **Trigger oscilloscope** | **StormX and up — built in** | explicitly listed |
| Nitrous, race timer, paddle shift, accelerometer, GPS | higher tiers | |
| Warranty | lifetime | |

**ARCHITECTURAL IDEA:** *Uniform capability floor* — 512 MB logging and 2 CAN buses on
every product, including the cheapest. This is a deliberate product decision that treats
data as a core deliverable rather than an accessory.

**ADVANTAGE:** Best feature density; the built-in trigger oscilloscope is the single most
copied idea in the enthusiast world; lifetime warranty indicates real field reliability.

**LIMITATION:** Closed; pricing/feature gating splits the range; touchscreen-free design
pushes dashboard work to the PC; a 100 kHz aggregate logging rate across 250 channels is
~400 Hz per channel, which is adequate but not premium for knock/knock-window work.

**RACCOON IMPLEMENTATION IDEA:** Adopt the trigger oscilloscope *as a first-class RACCOON
differentiator* (spec §128.4), rendered in the desktop app from ECU tooth timestamps, and
add the part Link does not have: **decoder state + sync confidence overlaid on the
oscilloscope**, so a user can see *why* sync was accepted or rejected.

---

## 7. FuelTech (PowerFT)

**SYSTEM:** FuelTech FT450 → FT700 PLUS.
**SOURCE:** <https://www.12voltmotorsports.com.au/pages/ecu-buyers-guide>; <https://www.12voltmotorsports.com.au/blogs/news/fueltech-ecu-australia-which-one-is-right-for-your-build>
**LICENSE:** Proprietary. Closed.

Published data (`VERIFIED-PUB` 2026-09-29, dealer-published):

| Capability | FT550 | FT600 | FT700 |
|---|---|---|---|
| Engine support | 1–12 cyl | 1–12 cyl | 1–16 cyl |
| Configurable inputs / outputs | 14 / 24 | 20 / 32 | 36 / 50 |
| Hi-Z injector outputs | 12 | 16 | 16 |
| Ignition outputs | up to 12 | up to 16 | up to 34 |
| CAN buses | 2 | 2 | 4 |
| Integrated touchscreen | 4.3" | 4.3" + 12 shift LEDs | 7" / 12.3" |
| DBW | yes | yes | yes |
| GearController (power shift) | yes | yes | yes |
| G-meter + gyro | yes | yes | yes + GPS |
| Onboard lambda conditioner | no | no | 2 |
| Internal MAP | 7 bar | 7 bar | 7 bar |
| Datalogger | 2 h 50 m | 617 h @ 1 kHz | 617 h @ 1 kHz |
| WiFi / Bluetooth | no | no | yes (encrypted) |
| Primary fuel map | — | 32×32 3D + per-cylinder trim | 32×32 3D |

**ARCHITECTURAL IDEA:** *ECU as dash* — the display, the shift lights, the gear
controller, the boost controller and the logger are one product in one box. For a
motorcycle, where there is no room for a separate dash, this is the correct physical
answer.

**ADVANTAGE:** The only reference system in this study that is *natively a motorcycle/drag*
product; drag-specific features (staging, burnout, 2-step, time-based launch) are
first-class; internal 7 bar MAP supports forced induction; 1 kHz × 617 h logging is the
best published endurance figure here.

**LIMITATION:** Touchscreen ECU UI cannot do dense table editing; 7-bar-class boost and
gear-controller hardware is overkill/expensive for a 2-cylinder street bike; a
screen-first product is a poor fit for map-level calibration.

**RACCOON IMPLEMENTATION IDEA:** Adopt *drag-mode first-class* (spec §42, §111) with
staging/burnout/2-step/time-based launch as a RACCOON profile, and adopt large-internal-map
support (V2 supports up to 32×32 configurable where memory allows, §24). RACCOON's UI
division of labour: **the laptop does dense calibration; the ECU does the race-critical
work and logs it.**

**RACCOON DIFFERENTIATOR:** We do not build a screen ECU in V2 (cost/thermal/optical
impact in an engine bay, and the spec's UI is a PyQt6 desktop application). We instead ship
a **CAN/analog dash output set** (spec §50, §51) so any dash can consume ECU data, and keep
the ECU silent. Documented as a deliberate non-goal: `docs/PRODUCT_REQUIREMENTS.md §NON-GOALS`.

---

## 8. MaxxECU

**SYSTEM:** MaxxECU MINI → PRO.
**SOURCE:** <https://www.maxxecu.com/ecu_compare>
**LICENSE:** Proprietary. Closed.

Published data (`VERIFIED-PUB` 2026-09-29, vendor self-published):

| Capability | Range |
|---|---|
| Sequential cylinders (max) | 4 (MINI) → 12 (PRO) |
| Wasted spark cylinders (max) | 8 → 12 |
| Wideband | 4.9/4.2 on all above MINI; dual 5.2/4.9/4.2 on Gen2 Track/Race |
| Internal log | 8 MB (most) → 8/16 GB (Gen2) |
| Max log rate | 1000 Hz |
| Logged channels | 170 → 2000+ (Gen2) |
| Embedded MAP | 400 kPa → 550 kPa |

**Notably: the entry-level MINI supports 4 sequential cylinders** — direct evidence for
our 1–4 cylinder product class as a commercially served segment. `VERIFIED-PUB`

**ARCHITECTURAL IDEA:** A single software across a hardware range from MINI to PRO, with
per-model channel counts (170 vs 2000+ channels).

**LIMITATION:** 170 channels at 1 kHz on most models is a real ceiling for combined
engine + chassis logging; 8 MB internal on most models is small.

**RACCOON IMPLEMENTATION IDEA:** Adopt tiering via capability model; set RACCOON's channel
count by measurement (spec §91), not by marketing.

---

## 9. ECUMaster, Syvecs, MoTeC, Bosch Motorsport, Marelli

**ECUMaster** — European motorsport-derived unit; the class of product closest to what
RACCOON is trying to be (professional, not hobbyist).
**LIMITATION / evidence status:** Publicly available material is marketing-level; detailed
architecture, timing, and protocol are not public. `UNKNOWN` — see `UNK-REF-002`.
**RACCOON USE:** confirms the existence of the professional market segment at ~₹100k class;
no architectural detail extracted; no code or design reuse.

**MoTeC** — professional motorsport ECU + **MoTeC i2** software, M1 ECU.
**Evidence status:** public material is product-level; internals not public. `UNKNOWN` — `UNK-REF-003`.
**Noted publicly:** MoTeC is the reference point for *professional motor racing data
workflow* (channels, context, session management, data acquisition engineer workflows).
**RACCOON USE:** the professional bar for the tuner experience and for logging
(per-session context, not just a CSV). No code/design reuse.

**Syvecs** — OEM/professional ECU manufacturer. `UNKNOWN` internally; product-level only.
**RACCOON USE:** confirmation that a hardware-abstraction-driven product family
(one core, many boards) is an industry-standard structure. No reuse.

**Bosch Motorsport** — MS-series ECUs, plus relevant *public* subsystem parts:
- **CJ125** — the dedicated LSU 4.9/4.2 wideband controller IC; used in Bosch's own
  LambdaTronic LT4. `VERIFIED-PUB` (<https://www.bosch-motorsport.com/products/sensors/lambda/lambda-sensor-lsu-4-9/>: "This lambda sensor operates only in combination with a special LSU-IC, used in most Bosch Motorsport ECUs and lambda control units like LT4").
- **Mini-LSU 4.9** — planar ZrO₂ dual-cell limiting-current sensor, monotonic output for λ = 0.65 → air, **requires the matched CJ125**. `VERIFIED-PUB` (same source). Note the same source warns amplification factor v=17 for lean / v=8 for rich — a hard analog fact for any LSU front-end design.
**RACCOON IMPLEMENTATION IDEA:** Wideband is specified as **CJ125 + LSU 4.9**, with the
pairing treated as a validated mated pair, and a documented calibration/units contract
(λ vs AFR vs O2 %) so the GUI never mislabels a value. Native wideband is **hardware-gated**
(spec §21) — CJ125 is an external component we can integrate on a wideband daughterboard,
not a capability we may *claim* without it.
**LIMITATION:** CJ125 is a restricted-supply part; availability and second sourcing are `UNKNOWN` → `UNK-HW-004`.

**Marelli (formerly Magneti Marelli)** — professional motorsport ECU supplier. `UNKNOWN`
internals. **RACCOON USE:** market/segment confirmation only.

---

## 10. Consolidated feature landscape

Legend: `●` published as supported, `◐` partial/limited, `—` published as absent,
`?` not established publicly.

| Capability | Speeduino | rusEFI | Haltech | Link | FuelTech | MaxxECU | RACCOON V2 target |
|---|---|---|---|---|---|---|---|
| 1–4 cylinder | ● | ● | ● | ● | ● | ● | ● (native, tested) |
| Sequential inj to 4 cyl | ● | ● | ● | ● | ● | ● | ● |
| VR crank conditioning in product | ◐ (external board) | ● | ● | ● | ● | ● | ● (in-product, protected) |
| Cam / phase | ● | ● | ● | ● | ● | ● | ● |
| Wasted spark | ◐ | ● | ● | ● | ● | ● | ● |
| Staged injection | ◐ | ● | ● | ● | ● | ● | ● |
| Closed loop (narrow) | ● | ● | ● | ● | ● | ● | ● |
| Onboard wideband | — | ● | ● (high tiers) | ◐ | ◐ (2 ch on FT700) | ● | ● via CJ125 daughterboard, capability-gated |
| Per-cylinder lambda | — | ◐ (dual AFR logging) | ◐ | ◐ | ◐ (FT600 cyl trim) | ◐ | ● 4 ch, capability-gated |
| Knock control | ● | ● | ● (high tiers) | ● (dual) | ? | ? | ● 2 ch + windows |
| VVT | — | — | ● 1–4 ch | ● | ? | ◐ | ● multi-ch |
| Boost control | — | ● | ● 3D/4D | ● | ● integrated | ? | ● |
| DBW | — | ● | ● | ● | ● | ? | ● capability-gated |
| Launch control | ◐ | ● | ● | ● | ● (time-based) | ● | ● |
| Flat shift | ◐ | ● | ● advanced | ● | ● gear ctrl | ● | ● |
| Traction control | — | ● | ● | ● | ● | ● | ● |
| Antiblag | — | ● | ● | ● | ? | ? | ● |
| Engine braking | — | ? | ? | ? | ? | ? | ● |
| Torque coordination layer | — | — | ◐ (torque mgmt) | ? | ? | ? | ● (central, mandatory) |
| Trigger oscilloscope | ◐ (tooth log) | ? | ? | ● | ? | ? | ● + decoder state |
| Internal logging | ◐ | ● | 2 MB–512 MB | 512 MB | 617 h @1 kHz | 8 MB–16 GB | ● target 512 MB–8 GB (measure) |
| Tune version control | — | — | — | — | — | — | ● (git-like, §57) |
| Fault *explanation* (what/why/evidence/test) | — | — | — | — | — | — | ● (§128.9) |
| 1–4 cyl native physics model | — | — | — | — | — | — | ● (engine geometry → volumetric model, §6/§93) |

The rightmost column is RACCOON's *design target*, not a claim of achievement. Every
`●` there is a requirement to be delivered and evidenced through the test ladder in
[`TEST_PLAN.md`](TEST_PLAN.md).

---

## 11. Architectural patterns RACCOON adopts (with attribution)

| # | Pattern | Learned from | RACCOON adoption | Where implemented |
|---|---|---|---|---|
| P1 | Compute once per crank revolution; schedule from nearest trigger event | rusEFI | Adopted verbatim in principle, hardened | `firmware/engine`, `firmware/scheduler` |
| P2 | Hybrid angle→time event offsets (not periodic timers) | rusEFI | Adopted + acceleration prediction | `docs/ENGINE_CONTROL_SPEC.md` §4 |
| P3 | Decoder abstraction with capability flags + shared core | Speeduino | Adopted, extended with a state machine | `firmware/trigger` |
| P4 | "Trigger angle after TDC" as the user-facing mental model | Speeduino | Adopted, plus verification tooling | `tuner/ui` TDC tool |
| P5 | Hardware capability model gating software | Haltech / MaxxECU / FuelTech | Adopted as signed, reported capability set | `firmware/can` capability frame, `tuner/core` |
| P6 | Uniform logging floor (data as a deliverable) | Link | Adopted, capacity set by measurement | `firmware/logging` |
| P7 | Trigger oscilloscope as a first-class product feature | Link | Adopted + decoder-state overlay | `tuner/ui` trigger scope |
| P8 | Analyser-as-product (offline statistical correction) | TunerStudio / MegaLogViewer | Adopted, made confidence-aware | `tuner/autotune` |
| P9 | Declarative UI definition (no hand-coded per-hardware forms) | TunerStudio | Adopted natively in PyQt6 from typed schema | `tuner/calibration` schema |
| P10 | Drag-specific first-class modes | FuelTech | Adopted (profile + torque manager), no screen | `firmware/torque`, `firmware/launch` |
| P11 | Mated LSU 4.9 + CJ125 wideband | Bosch Motorsport | Adopted as hardware-gated capability | `hardware_profiles` + `firmware/lambda` |
| P12 | Uniform logging + a documented, shared sensor/transfer-function library | rusEFI / Speeduino sensor support | Adopted | `firmware/sensors`, `tuner/ui` sensor editor |
| P13 | Single configuration source of truth | rusEFI (`rusefi_config.txt`) | Adopted the *discipline*, rejected the mechanism (text + page-size lock) | `tuner/calibration` schema, `tools/codegen` |

---

## 12. Explicit non-adoptions

| Rejected | From | Reason |
|---|---|---|
| Any GPL code (Speeduino GPL-2.0, rusEFI GPL-3.0+terms, MS family) | Speeduino / rusEFI / MegaSquirt | Copyleft would require releasing RACCOON firmware source. Clean-room authored instead. See `THIRD_PARTY_LICENSE_REGISTER.md`. |
| TunerStudio/MegaLogViewer code or ini-art pipeline | EFI Analytics | Proprietary; reverse-engineering and repackaging explicitly prohibited by its licence. |
| Text-generated config with hard-coded page sizes | rusEFI | Fragile coordination between firmware, generator, and UI. |
| Scripting (Lua) or dynamic load in the engine-control core | rusEFI | Violates RACCOON §104/§103 determinism rules. |
| Screen/touch ECU in V2 | FuelTech | Out of the specified product form (§61 is a desktop application); adds cost, thermal load, and glare in an engine bay. Becomes a documented non-goal with a CAN/analog dash-output substitute. |
| 5 V default sensor domain | most | RACCOON standardises on a protected 5 V *and* 3.3 V-capable input front end per channel, because the target engine is 12 V and modules are mixed. |

---

## 13. Evidence gaps in this document

- MegaSquirt/MS3 details are community-level, not primary-source verified → `UNK-REF-001`.
- ECUMaster, MoTeC, Syvecs, Marelli internals are not public → `UNK-REF-002`, `UNK-REF-003`.
- All vendor performance/feature claims are vendor self-published; they are used for
  *capability landscape* only and are never treated as engineering evidence for RACCOON.
- Feature matrices in this document describe *published* product lines as of
  2026-09-29 and will drift. They are not requirements.
