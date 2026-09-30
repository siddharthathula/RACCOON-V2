# MCU SELECTION

**Document ID:** MCU-001
**Status:** PHASE 1 — recommendation, pending design review (`UNK-HW-001`)
**Principle (spec §130.D):** selection is driven by **timer architecture, ADC, DMA, CAN,
memory, package, cost, availability, real-time capability, automotive suitability** — not
by clock speed.

---

## 1. Requirements derived from the product, not from a wish list

### 1.1 Timing requirements (these drive everything)

| # | Requirement | Number | Where from |
|---|---|---|---|
| T1 | Capture two position-sensor inputs (CKP, CMP) with sub-microsecond resolution and no ambiguity across a full stop-to-overspeed sweep | 32-bit capture, 1 µs tick ⇒ 4295 s wrap at 1 µs | §8, §9, §13 |
| T2 | Injector pulse edges with ≤ ±5 µs error at the design maximum | 8 injector channels (4 primary + 4 secondary) | §28, §29 |
| T3 | Ignition charge-start and spark-stop edges with ≤ ±5 µs error | 4–8 ignition channels | §30–§33 |
| T4 | Independent hardware output for: VVT (1–2), IAC PWM, stepper (2 coils), boost solenoid, fuel pump PWM, aux PWM ×4, tach, shift light | ≥ 12 further channels | §36–§50 |
| T5 | A free-running 1 µs timebase independent of any channel | 1 | §91 |
| T6 | Hardware fault inputs (e.g. injector current-limit fault, over-voltage) able to disable outputs **without CPU involvement** | ≥ 1 | §103, §105 |
| T7 | Timer channels must be assignable so that a 4-cylinder sequential build is possible on the smallest variant | see `IO_MATRIX.md` | §89 |

**Design conclusion:** the actuator edges must be produced by **timer hardware**, with the
CPU only loading compare registers. Software toggling a GPIO can never satisfy T2/T3.
This is a hard architectural constraint, not an optimisation.

### 1.2 Acquisition requirements

| # | Requirement | Number |
|---|---|---|
| A1 | Analog sensor inputs with simultaneous sampling for redundant pairs (TPS1/TPS2) | ≥ 2 ADC capable of simultaneous/dual-mode sampling |
| A2 | Deterministic sample instant relative to the crank (so load and speed are phase-aligned) | ADC triggered by a timer, DMA circular |
| A3 | Sensor sampling rate | 1–2 kHz sufficient; 5 kHz for knock/EGT if fitted |
| A4 | Effective resolution | 12-bit native, oversampling acceptable |
| A5 | Ability to time two signals (knock conditioning window, EGT) | comparators/timers |

### 1.3 Communication, storage, security

| # | Requirement | Number |
|---|---|---|
| C1 | CAN 2.0B, prefer CAN-FD for future | ≥ 2 FDCAN |
| C2 | USB (CDC or MSC) for the tuner | 1 USB FS or HS |
| C3 | 512 MB – 8 GB internal logging | external storage controller (SDIO/QSPI) |
| C4 | Signed firmware, authenticated calibration, unique identity | hardware crypto + unique ID + secure flash |
| C5 | A/B (dual-bank) flash for a non-bricking update | dual-bank flash or an external NOR |
| C6 | Watchdog(s) with independent domains | 2 |

### 1.4 Real-time and verification

| # | Requirement |
|---|---|
| R1 | Deterministic, provable execution. No dynamic allocation, no blocking, no priority inversion. |
| R2 | Host-compilable engine core (so the same code is unit-tested and SIL'd on a PC). |
| R3 | On-chip cycle counter + GPIO timing capability to *measure* ISR latency (§91). |
| R4 | ECC/parity on memories, or at minimum ECC on flash, for detecting memory corruption (§90). |
| R5 | Debug port that can be permanently disabled in production (§60, §106). |

---

## 2. Candidate analysis

### 2.1 STM32H733 / H725 (single Cortex-M7, 550 MHz)

| Criterion | Assessment |
|---|---|
| Core | Cortex-M7 @550 MHz, DP-FPU, cache. Vendor claim 2778 CoreMark. Source: ST H7 overview collateral (`VERIFIED-PUB`) |
| Flash / RAM | up to 1 MB flash (dual-bank capable), 564 KB RAM incl. 128 KB DTCM + 432 KB system. `VERIFIED-PUB` |
| ADC | 2 × 16-bit @3.6 MSPS + 1 × 12-bit @5 MSPS. `VERIFIED-PUB` |
| CAN | 1 TT/FD-CAN + 2 FD-CAN = 3. `VERIFIED-PUB` (H725/H735 line) |
| Timers | 4 × 32-bit advanced, 5 × 16-bit LP, 12 × 16-bit TIM, 2 watchdogs. `VERIFIED-PUB` |
| Crypto/security | Crypto accelerator + hash, TRNG, secure services (SFI, SB-SFU) on CPN part numbers, anti-tamper, unique ID, **ECC on all memories**. `VERIFIED-PUB` |
| USB | 1 × USB OTG HS/FS. `VERIFIED-PUB` |
| Storage | SDMMC ×2, Octo-SPI, Quad-SPI, PSSI. `VERIFIED-PUB` |
| Math | Cordic + FMAC. `VERIFIED-PUB` |
| Package | QFN68 / LQFP100 / etc. |
| Determinism | M7 with cache requires cache management discipline (lock critical sections / use ITCM-DTCM); achievable but must be designed for, not discovered later |
| Cost/availability | Mainstream, plentiful, ST tooling (CubeMX, CubeIDE), HAL Apache-2.0 |
| Automotive | Extended temperature variants exist; **not** ASIL-rated in the H7 general-purpose line |

**Verdict:** **Primary MCU.** Meets every hard requirement with headroom (1 MB flash for a
full-featured 4-cylinder build; 564 KB RAM; 3 CAN; 3 ADC; ECC on memory). Single core
simplifies the real-time proof, which we weight heavily (§103, §104, §133).

### 2.2 STM32H745 / H755 (dual core: M7 + M4)

**Verdict:** **Rejected for V2, retained as an option for a future "V2 Pro".**
Rationale: the M4 is attractive for offloading comms/logging from the engine core, which
directly serves §104. But it doubles the certification/test surface (two cores, inter-core
IPC, coherency, two watchdog domains) and the M7 at 550 MHz is not the bottleneck for a
1–4 cylinder engine. The determinism argument in `FIRMWARE_ARCHITECTURE.md` is that a
**single core with a static schedule** is easier to prove correct than a dual-core system
with a fast core and a slow core. Complexity is a safety risk here, not a feature.

### 2.3 STM32G474 (Cortex-M4, 170 MHz)

| Criterion | Assessment |
|---|---|
| Core | M4 @170 MHz, 213 DMIPS / 550 CoreMark, ART accelerator, 32 KB CCM-SRAM routine booster, Cordic + FMAC. `VERIFIED-PUB` |
| Flash / RAM | 512 KB dual-bank (G474) with ECC, 96 KB SRAM + 32 KB CCM. `VERIFIED-PUB` |
| ADC | up to 5 × 12-bit @4 MSPS with hardware oversampling, 20 ns aperture, simultaneous sampling. `VERIFIED-PUB` |
| Analog | 6 op-amps with PGA, 7 comparators (17 ns), 7 DACs. `VERIFIED-PUB` |
| CAN | up to 3 × FDCAN. `VERIFIED-PUB` |
| Timer | 3 × advanced motor-control, 2 × 32-bit, 1 × HRTIM 12-ch @184 ps. `VERIFIED-PUB` |
| Crypto | AES-256 hardware, securable memory area, dual-bank flash with ECC, SRAM parity. `VERIFIED-PUB` |
| Strength | Best analog front end per euro; on-chip op-amps can condition signals and reduce BOM |
| Weakness | 512 KB flash is **tight** for: bootloader + 2× application + bootloader copy + large maps (20×20 or 32×32 tables, multi-cylinder) + logging index. CCM 32 KB is very small for a 4-cyl engine state |
| Verdict | **Cost-down variant** for a single/2-cylinder build with reduced features — but *not* the primary. Retain as `hardware_profiles/mcu/stm32g474.json` so the same firmware can target it with feature gating. This is exactly the "universal capability model" the spec demands (§94). |

### 2.4 Renesas RH850 (G4MH automotive family)

| Criterion | Assessment |
|---|---|
| Strength | Purpose-built automotive; ISO 26262-capable variants (ASIL-B/D), lockstep cores, hardware ECC, CAN-FD/LIN, ASIL-capable timer/ADC |
| Verdict | **Deferred / premium option.** It is the *right* answer if RACCOON ever needs road-homologation or ASIL claims. For a motorsport aftermarket ECU, cost, tooling (Grenadier, proprietary debugger), and NRE dominate. Not a V2 dependency. `PROSPECTIVE`. |

### 2.5 NXP S32K / S32G

| Criterion | Assessment |
|---|---|
| Strength | Automotive grade, ASIL options, excellent CAN-FD and safety hardware, Lockstep Cortex-M7 in some parts |
| Verdict | Credible automotive alternative; peripheral set is smaller than STM32H7 for the analog/timer mix we need. `PROSPECTIVE` — keep as a portability target of the HAL boundary, not a V2 dependency. |

### 2.6 Non-STM32 hobby-class (ESP32, RP2040, Teensy/ATmega)

**Rejected.** Insufficient timer/ADC/CAN resources and no security/ECC story; Teensy/ATmega
form factors are the Speeduino class. Would cap the product at "enthusiast" and violate
the motorsport positioning.

---

## 3. Decision

```
PRIMARY      : STM32H725 / H733  (Cortex-M7 550 MHz, 1 MB dual-bank flash, 564 KB RAM,
                                    3× FDCAN, 3× ADC, crypto+ECC, USB HS, SDMMC/OctoSPI)
COST-DOWN    : STM32G474          (512 KB dual-bank flash, 96+32 KB RAM, 5× ADC, up to 3× FDCAN)
PROSPECTIVE  : RH850 (automotive homologation), S32G (safety portfolio)
EXCLUDED     : dual-core H745 (V2), G4 low-density (G431) (insufficient memory),
               hobby-class MCUs
```

**One engine core, two hardware profiles.** The engine core is written in portable C++17
against a `hardware_profile` table (ADC map, timer map, GPIO map, CAN, storage), so the
same binary source builds for both, and the capability set reported over the protocol
differs. This is the concrete implementation of spec §92 and §94.

**Unresolved → `UNK-REF-005`:** the exact FDCAN instance count for the *specific* orderable
part number must be read from that part's datasheet revision before the pin map is frozen.
Vendor collateral in the public sources used here is internally inconsistent on this
point (2 vs 3 FDCAN for H74x/H75x lines), and the I/O matrix assumes **3 available, 2 used**.

---

## 4. Why this MCU, in one paragraph

A 550 MHz M7 with 564 KB of RAM, ECC on all memories, three ADCs, three FDCAN, hardware
crypto and dual-bank flash gives us enough *headroom* to buy three things that matter more
than raw speed: (a) room for a large calibration schema plus a full bootloader plus a
rollback slot, (b) room for two application tasks that are provably not interfering — a
static scheduler with no blocking — and (c) the peripherals to produce every actuator edge
in hardware. The single most important selection criterion was **T2/T3**: the ability to
schedule injector and ignition edges in timer hardware rather than in software, because no
amount of CPU speed makes a software-toggled edge deterministic enough for motorsport fuel
and spark control.
