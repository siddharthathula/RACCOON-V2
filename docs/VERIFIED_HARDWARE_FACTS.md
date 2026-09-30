# VERIFIED HARDWARE FACTS AND ARCHITECTURE CORRECTIONS

**Document ID:** VHF-001 · **Date:** 2026-09-29 · **Supersedes:** parts of `MCU_SELECTION.md`, `IO_MATRIX.md`, `REFERENCE_ECU_RESEARCH.md`

This document records **independent web verification of the three architecture inputs added
on 2026-09-29** (ESP32-S3 wireless comms, "configure the bike in the GUI then push it to
the ECU", signed/encrypted tune enforcement), together with **corrections to claims made
in the Phase 1 documents**.

Verification labels: `[VERIFIED: source, date]` · `[UNKNOWN]` · `[CALCULATED]`.

---

## 0. CORRECTIONS — what Phase 1 got wrong

These are recorded first because code must not be built on them.

| # | Phase 1 claim | Verdict | Correction |
|---|---|---|---|
| C-01 | STM32H725 has "hardware AES/SHA" crypto | **WRONG** | **The H725 has no crypto accelerator at all.** ST DS13311 Table 2: `Cryptographic accelerator: no`; its CMSIS header defines no `CRYP`/`HASH` peripheral. **The H733 does have one** (ST markets it as a "subset of peripherals including a Crypto accelerator", with SFI/SB-SFU bootloader support). `[VERIFIED: st.com DS13311 Rev5, cmsis_device_h7, 2026-09-29]` |
| C-02 | H725 has "TrustZone" | **WRONG** | No TrustZone reference in DS13311; no TZPC/SAU in the device header. TrustZone on H733 is `[UNKNOWN]` — verify explicitly. `[VERIFIED: DS13311 + cmsis_device_h7, 2026-09-29]` |
| C-03 | H725 flash is "dual-bank (A/B)" | **WRONG** | H725 has **one** user flash block of eight 128 KB sectors, no bank swap. A/B must be **sector-granular**. `[VERIFIED: DS13311 §3.3.1, 2026-09-29]` The G474 *does* have true dual-bank with read-while-write, and so does the H7A3. |
| C-04 | H7-class has "3 FDCAN" or "2 FDCAN" (we noted the conflict) | **RESOLVED: 3** | FDCAN1/2/3 exist; **only FDCAN1 supports TT-CAN (ISO 11898-4)**. The "2" figure is an internal error in ST's own summary table (§3.42 and the CMSIS header both say three). ⚠ Message RAM is **10 KB shared across all three**. `[VERIFIED: DS13311 §3.42, 2026-09-29]` |
| C-05 | "4 × 32-bit advanced timers" | **WRONG — there are none** | TIM1/TIM8 are advanced-control with **16-bit** counters. The 32-bit timers are four *general purpose*: **TIM2, TIM5, TIM23, TIM24**. All long-period scheduling must use those. Total compare channels across all timers ≈ 40. `[VERIFIED: DS13311 Table 5 / §3.31, 2026-09-29]` |
| C-06 | "3 identical ADCs" | **MISLEADING** | ADC1/ADC2 are 16-bit capable (3.6 MSPS @16-bit, simultaneous + interleaved S/H, hardware analog watchdog, timer-triggered). **ADC3 is 12-bit only** (5 MSPS). Hardware oversampling is **not stated** in DS13311 → `[UNKNOWN]`, confirm in RM0468. `[VERIFIED: DS13311 §3.19, 2026-09-29]` |
| C-07 | "LQFP100 and QFN68" as the package shortlist | **QFN68 is unsuitable** | VFQFPN68 is **DCDC-only (no LDO)**, 46 I/O, no ULPI, no Ethernet. LQFP100 = 67 I/O is *too small* for a 30-channel timer budget plus 16 analog inputs. **Use LQFP144 (97 I/O) or LQFP176 (119 I/O).** `[VERIFIED: DS13311 Table 2, 2026-09-29]` |
| C-08 | "564 KB RAM" as a flat figure | **Upper bound only** | 564 KB is the maximum across the family; per-package SRAM varies (AXI 128–320 KB, ITCM 64–256 KB, DTCM 128 KB, backup 4 KB). `[VERIFIED: DS13311 §3.3.2, 2026-09-29]` |
| C-09 | NXP MC33810 / MC33813 are candidate power-stage ICs | **BOTH END OF LIFE** | NXP's own product pages carry an "End of Life" banner on both. MC33813's successor **MC33814 is Active** (2-cylinder small-engine IC, 1 injector driver + relays). Note MC33813 offered only **one** injector driver, so it was the wrong part class anyway. `[VERIFIED: nxp.com MC33810 / MC33813, 2026-09-29]` → see §5.3 |
| C-10 | "ISO 7637-2 covers load dump" | **OUT OF DATE** | ISO 7637-2:2011's scope explicitly excludes pulses 4, 5a, 5b — those are now in **ISO 16750-2 / ISO 21848**. Any test spec citing "ISO 7637-2 pulse 5" is using the 2004/2008 edition. `[VERIFIED: ISO 7637-2:2011 scope statement, 2026-09-29]` |
| C-11 | "Speeduino generates source and flashes per vehicle" | **WRONG (now)** | Speeduino ships **one prebuilt firmware per MCU**; all vehicle configuration is **data** written to EEPROM over the serial protocol. Source generation was the 2013–2016 model and was abandoned. `[VERIFIED: SpeedyLoader README "avoids the need for any compiling"; speeduino/storage.h; 2026-09-29]` → see §2 |
| C-12 | "Dropbear" as a VR conditioner board | **WRONG** | **Dropbear is a complete 8+8 Speeduino ECU board** (Teensy 3.5; v2 = Teensy 4.1, adds Wi-Fi + BLE), designed by Josh Stewart, CERN-OHL-S, with an *onboard* VR conditioner and SD logging. `[VERIFIED: wiki.speeduino.com/en/boards/dropbear + github.com/speeduino/Hardware, 2026-09-29]` |
| C-13 | "MC33810 gives spark-duration (SPKDUR) feedback we should use" | **Feature real, part dead** | The datasheet feature is genuine, but the part is EOL. A discrete design must reproduce the function (a primary-side divider sensing the flyback) — see §5.3. `[VERIFIED: MC33810 datasheet Rev 11.0 + NXP EOL banner, 2026-09-29]` |

---

## 1. NEW INPUT #1 — ESP32-S3 as the wireless comms MCU

### 1.1 Verified silicon

| Item | Value | Source |
|---|---|---|
| CPU | Dual-core Xtensa LX7, single-precision FPU, **240 MHz** | `[VERIFIED: esp32-s3_datasheet_en.pdf]` |
| SRAM | **512 KB** + 16 KB RTC SRAM; PSRAM supported (Quad/Octal) | `[VERIFIED: datasheet + esp-idf v6.1 soc_caps.h]` |
| Wi-Fi | 802.11b/g/n, **2.4 GHz only**, 1T1R 150 Mbps, up to +19.5 dBm | `[VERIFIED: datasheet]` |
| BLE | Bluetooth 5 LE + Mesh, 125/500 kbps, 1 Mbps, 2 Mbps, −104.5 dBm RX | `[VERIFIED: datasheet]` |
| USB | **USB-Serial-JTAG** (ROM CDC, no code needed) **and** full-speed **USB-OTG** — ⚠ **both share one internal PHY, only one can run at a time** | `[VERIFIED: esp-idf v6.1 usb_device docs]` |
| UART | 3 × UART, max **5 Mbaud** | `[VERIFIED: soc_caps.h]` |
| SPI | 2 general-purpose SPI (+2 for flash/PSRAM), DMA | `[VERIFIED: datasheet + soc_caps.h]` |
| CAN | **1 × TWAI** (CAN 2.0) | `[VERIFIED: soc_caps.h]` |
| SDIO | **ABSENT** on S3 (has 2 SD/MMC host slots instead) | `[VERIFIED: soc_caps.h]` |
| Crypto | HW **AES-128/256**, **SHA-1/224/256/384/512**, **RSA up to 4096-bit**, **HMAC**, **TRNG** | `[VERIFIED: datasheet + soc_caps.h]` |
| ECC | **No ECC accelerator** → RSA-PSS only for signatures | `[VERIFIED: no SOC_ECC_* for esp32s3 in v6.1 soc_caps.h]` |
| eFuse | 4096 bits total, ~1792 user-usable, per-field read/write protection | `[VERIFIED: flash-encryption + secure-boot-v2 docs]` |
| SDK | **ESP-IDF v6.1** (tagged 27 Aug 2026, marked Latest). v5.2.x is EOL. | `[VERIFIED: github.com/espressif/esp-idf/releases]` |

### 1.2 Verified security capabilities — this is the decisive part

| Feature | Verdict | Source |
|---|---|---|
| **Hardware root of trust** | **Yes.** The trust anchor is the **SHA-256 digest of an RSA-3072 public key in an eFuse**, verified by the **immutable ROM bootloader** before the 2nd stage runs. The private key never touches the device. | `[VERIFIED: secure-boot-v2 docs]` |
| Secure Boot v2 | **RSA-3072, RSA-PSS only**, signature block v`0x02`, 3 revocable key digests, apps re-verified on every OTA, fallback to another valid image on failure | `[VERIFIED: secure-boot-v2 docs]` |
| Flash Encryption | **XTS-AES-128/256 in hardware**; key generated on first boot from the TRNG, stored read+write protected so **software cannot access it** | `[VERIFIED: flash-encryption docs]` |
| **Signed data partitions** | **This is the mechanism for signed calibration blobs.** `CONFIG_SECURE_SIGNED_DATA_PARTITION` verifies data-partition images with the *same* RSA-PSS secure-boot mechanism. `idf.py secure-sign-data`. | `[VERIFIED: ota API reference + secure-boot-v2 docs]` |
| App rollback | **Enabled by default**; `esp_ota_mark_app_valid_cancel_rollback()` / `..._invalid_rollback_and_reboot()` | `[VERIFIED: ota docs]` |
| Anti-rollback | eFuse security version, **16-bit → max 16 steps** | `[VERIFIED: ota docs]` |
| eFuse budget | FE(AES-128) + SB(RSA-3072) + controls ≈ **533 of 1792 user bits** | `[CALCULATED from verified eFuse tables]` |
| ⚠ Enable order | **Flash Encryption FIRST, then Secure Boot.** Wrong order burns `RD_DIS` and makes the FE key permanently unreadable. | `[VERIFIED: security-features-enablement-workflows]` |
| ⚠ What FE does *not* do | Integrity/authenticity, flash layout (identical plaintext → identical ciphertext), RAM contents, or anything on the STM32 link. Secure Boot covers the first. | `[VERIFIED: flash-encryption docs]` |
| ⚠ Consequence | Enabling Secure Boot / FE **disables the ROM USB stack** (no serial emulation, no DFU on that port). | `[VERIFIED: secure-boot-v2 docs]` |

### 1.3 Modules and thermal gotchas that affect the ECU

| Part | Flash/PSRAM | Temp | Price (Espressif sample, USD) |
|---|---|---|---|
| **ESP32-S3-WROOM-1-N8R2** | 8 MB / 2 MB **Quad** | **−40…85 °C** | **$3.35** |
| ESP32-S3-WROOM-1-N8 | 8 MB / — | −40…85 °C | $3.20 |
| ESP32-S3-WROOM-1-N8R8 | 8 MB / 8 MB **Octal** | **−40…65 °C** ⚠ | $3.62 |
| ESP32-S3-WROOM-1U-N8R2 | 8 / 2, **external antenna (U-FL)** | −40…85 °C | ~$3.35 |

`[VERIFIED: esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf + espressif.com module pages]`

⚠ **Decisive thermal constraint:** all **Octal-PSRAM** modules (any `R8`, `R16`, and every
`WROOM-2`) are rated **only to 65 °C**. A motorcycle ECU near an engine or under a seat
makes that a field-failure mechanism. **Select `N8R2` (Quad PSRAM, 85 °C) or `N8`, and
prefer the `-1U` variant (external antenna) so the RF can be placed away from the ECU.**
Also: Octal-PSRAM modules consume GPIO35/36/37.

`[VERIFIED: WROOM-1/WROOM-1U datasheet, 2026-09-29]`

### 1.4 Power — the real number

| Mode | Typical |
|---|---|
| Wi-Fi TX (802.11b, 20.5 dBm) | **355 mA** |
| Wi-Fi RX | 95 mA |
| Modem-sleep @240 MHz, dual-core idle | **32.9–47.6 mA** (CPU still running!) |
| Light-sleep | 240 µA |
| Deep-sleep | 7–8 µA |
| Shutdown | 1 µA |

`[VERIFIED: WROOM-1 datasheet]` · ESP-IDF measured idle 23.88 mA `[VERIFIED: current-consumption docs]`

**Design consequences (all binding):**
1. The 3.3 V rail must survive 355 mA plus cranking/load-dump transients. This is a
   *power-supply* requirement, not a radio one.
2. "Modem-sleep" is a trap by name — at 240 MHz the CPU is still burning ~30–48 mA. Idle
   firmware must **drop the clock and use automatic light sleep**, or the module becomes a
   150 mW heater in a sealed box.
3. **Flash writes completely suspend task execution** — any OTA staging on the same flash
   stalls the STM32 link. Stage OTA on external NOR, or schedule updates only when the
   engine is off.
4. Power-gating is viable (1 µA shutdown, RTC retained) but every power cycle re-pays
   association latency, which Espressif does **not** publish → **measure it yourself**.

### 1.5 Wireless: what is and is not acceptable

| Use | Verdict | Why |
|---|---|---|
| Live telemetry / log download | **Acceptable** | Espressif's own numbers: UDP TX 30 Mbit/s in open air vs 98 Mbit/s shielded — a 3× RF-environment penalty. The throughput loss matters more than the latency for these uses. `[VERIFIED: wifi-performance docs]` |
| Signed firmware / calibration OTA | **Acceptable** | With app-controlled rollback so a bad image reverts on the next boot. |
| Tuning *configuration* writes | **Acceptable with a gate** | Writes must be idempotent + sequenced + confirmed, because Wi-Fi ARQ is invisible to the application. |
| Anything inside a fuel/ignition/throttle loop | **Never** | Hard architectural boundary. |

⚠ **WIFI_PS_MIN_MODEM is the ESP-IDF default** and adds DTIM-synchronous receive latency;
with BLE running, coexistence time-slicing removes the `WIFI_PS_NONE` benefit.
`[VERIFIED: wifi-performance-and-power-save docs]`

**Industry precedent, and it validates the design:** Haltech Nexus hosts its **own SoftAP**
with a user-set SSID/password and keeps USB-C as the primary/recovery path; FuelTech FT700
offers **encrypted** Wi-Fi with up to 3 simultaneous clients and full function parity;
MaxxECU uses Bluetooth for display and USB for tuning; MoTeC deliberately uses licensed
cellular/radio spectrum for *trackside* telemetry rather than 2.4 GHz Wi-Fi.
`[VERIFIED: hpacademy.com, fueltech.com.br, maxxecu.com, motec.com.au, 2026-09-29]`

**Conclusion:** Wi-Fi is a *workshop/garage/bench* transport. It must never be the only path,
and it must never carry a deadline.

### 1.6 Alternative considered

**ESP32-C5** is the only part with 2.4 **and** 5 GHz dual-band Wi-Fi 6, and Espressif's own
page names the exact use case: *"For customers who would like to use ESP32-C5 as a
communication co-processor with an external host…"* — but it has 29 GPIO, 384 KB SRAM, and
its **ECDSA secure boot is disabled in IDF v6.1** for a security vulnerability (RSA only).
`[VERIFIED: espressif.com/products/socs/esp32-c5 + esp-idf v6.1 breaking changes]`
**Decision: ESP32-S3 for V2; C5 recorded as the V3 candidate if 2.4 GHz congestion proves
a real field problem.**

---

## 2. NEW INPUT #2 — "configure the bike in the GUI, then push it to the ECU"

### 2.1 The finding that shapes the design

Speeduino's real model is:

```
one prebuilt firmware per MCU  +  ALL vehicle configuration as DATA
```

- SpeedyLoader's own README: *"It avoids the need for any compiling and allows the user to
  select a firmware version and then automatically downloads a pre-compiled version."*
- Board selection is by **USB VID/PID auto-detection**, not by the user.
- Vehicle configuration lives in **TunerStudio config pages** persisted to EEPROM.
- `speeduino/storage.h`: *"A 'page' is a TunerStudio concept… **It's basically the tune,
  minus calibration tables.**"*
- Pin assignment is **data** (`src/pins/pinMapping.h`), not `#define`s.
- **Source generation was the 2013–2016 model and was abandoned.** The 2016 `speeduino.ino`
  had a hand-edited `Config section` with `#define engineSquirtsPerCycle 2`,
  `#define MAX_RPM 18000`, and hand-edited `page_size = 64` / `displaySignature` constants
  in `globals.h`. All gone today.
  `[VERIFIED: github.com/speeduino, tags 201612 → master, 2026-09-29]`

**Dropbear (what the user named) is the embodiment of this**: a complete 8-injector +
8-ignition Speeduino board, CERN-OHL-S, designed by Josh Stewart, with an onboard VR
conditioner, onboard MAP/baro, SD logging, and — in v2 — **Wi-Fi and BLE** on a Teensy 4.1.
`[VERIFIED: wiki.speeduino.com/en/boards/dropbear, github.com/speeduino/Hardware, 2026-09-29]`

**So the user's instinct is right about the product and slightly off about the mechanism.**
The correct RACCOON interpretation:

> **The GUI is where a bike is defined, and finishing a bike means writing that definition
> to the ECU — as a validated, versioned, signed, transactional data package, not as
> recompiled firmware.**

This is *better* than the old model and is what every commercial ECU does (§2.3).

### 2.2 The failure modes this replaces (all documented in the wild)

| Failure | Evidence |
|---|---|
| **Firmware/ini signature skew.** The GUI's data-layout description and the firmware's are two hand-edited files; in Speeduino they are `codeVersion[]` in `comms.cpp` and the `signature` string in a 477 KB hand-maintained `.ini`, with **zero `static_assert`s** in `config_pages.h` and only a *syntax* validator in CI | `[VERIFIED: speeduino/comms.cpp, reference/speeduino.ini, .github/workflows/validate-ini.yml]` |
| **A shipped base tune with the wrong signature.** SpeedyLoader issue #42, open since 2024-03-06: a downloaded base tune carried `firmwareInfo="Speeduino+2023.10"` when 2024.02 was requested | `[VERIFIED: github.com/speeduino/SpeedyLoader/issues/42]` |
| **Values that exist in the project but not the tune.** Forum: *"Those values aren't saved in tune… Others are just used by TS to calculate req. fuel."* ⇒ the GUI's displayed model ≠ the ECU's model | `[VERIFIED: speeduino.com/forum/viewtopic.php?t=4072]` |
| **Calibration silently changes meaning across a firmware update.** Link G4X: *"Fixed GP RPM Calibration, was actually revolutions per second… **IF UPDATING TO THIS FIRMWARE FROM AN OLDER ONE… YOU WILL NEED TO CHECK YOUR CALIBRATION.**"* MaxxECU: *"this is because 1.108 does not support the dynamic 4D tables that were added in 1.109"* | `[VERIFIED: linkecu.com G4X update PDFs; maxxecu.com/mtune]` |
| **Toolchain burden of source generation.** SpeedyLoader's answer: bundle five avrdude + four dfu-util builds → a **104–156 MB** installer that scales with every MCU family | `[VERIFIED: github.com/speeduino/SpeedyLoader release assets]` |
| **Bricking.** *"electrical noise reboot during settings change causes with full tune loss"* (rusEFI changelog 2019); Teensy 4.1 *"USB connection will cause lockup and soft-brick"* (issue #847) | `[VERIFIED: rusefi changelog; speeduino issue #847]` |
| **A code regression silently invalidating a hardware revision's pin map.** Speeduino `202501.3` exists only to fix *"a single regression that caused CAN to stop working on Dropbear v2 units"* | `[VERIFIED: github.com/speeduino/speeduino/releases]` |

### 2.3 What commercial ECUs do (unanimous, and it is the answer)

Configuration is **data**; firmware is a **separate, user-triggered operation**.

| Vendor | Configuration | Firmware |
|---|---|---|
| **Link G4X** | `.pclx` base-configuration file, "Open" to write; separate `Store` (F4) to persist | Separate: *"Update Firmware" under 'ECU Controls'* |
| **Haltech** | Setup wizard + "Plug-in Information → Model", which *"sets the hardware configuration on the ECU"*; and a **low-power mode where *"the ECU's inputs and outputs are disabled, allowing you to safely configure your vehicle setup before installation"*** | Separate: *"Tools → Firmware Update"*; and NHRA-class-specific firmware builds |
| **MaxxECU** | Tune is a plain file; *"Live tuning — No need to burn changes"* | Separate: *"Configuration → Manual Firmware Update"*; note PDM modules have **their own firmware namespace** |
| **FuelTech** | Map file + *"click Write ECU"*; ships a **base-map generator** (generates a map, not a build) | Separate: *"Tools → ECU Updater"* |
| **ECUMaster** | *"The project is a tune file that contains all calibrations, settings, maps and user defined objects. **The project can be password protected.**"* | Separate: *"Upgrade firmware"* |
| **MoTeC** | Base configuration data; regulatory/class-specific firmware builds | Separate |

`[VERIFIED: linkecu.com HVLCX.pdf, support.haltech.com, maxxecu.com/webhelp, files.fueltech.net, ecumaster.com EMU_PRO guide, 2026-09-29]`

**The single best industry idea to copy** (from rusEFI): the ECU presents itself as a USB
mass-storage device containing the matching definition file, so *"you never have to go
fishing for it again"* — a **ramdisk image generated at build time** containing the config
descriptor.
`[VERIFIED: wiki.rusefi.com HOWTO-create-tunerstudio-project; rusefi gen_config outputs ramdisk_image.h]`

---

## 3. NEW INPUT #3 — "ask for encryption for unknown tunes"

### 3.1 Interpretation adopted

The request is implemented as a **two-layer gate**, which is what actually protects a
user and satisfies §59/§60:

```
Layer 1 — SIGNATURE (authenticity)
  A tune/configuration written by RACCOON carries an RSA-3072 signature produced by the
  release key. The ESP32-S3 verifies it against the eFuse-anchored public-key digest
  (hardware root of trust, ROM bootloader).

Layer 2 — CREDENTIAL PROMPT (authorisation) for UNKNOWN tunes
  A tune with NO valid RACCOON signature is an UNTRUSTED TUNE. Writing or activating one
  requires, in the GUI:
     • an explicit dialog naming the risk,
     • an explicit engineering credential (never a shared development password),
     • an audit-log entry.
  On the ECU, an untrusted tune may be written ONLY to the inactive slot, can never be
  auto-activated at boot, and CANNOT arm engine control until it has been explicitly
  acknowledged with a valid session.
```

Why both layers: a signature says "RACCOON produced this"; the credential says "this human
accepts responsibility". A third-party tuner who bought the product legitimately cannot
obtain RACCOON's signing key, so a *legitimate* tune would be permanently untrusted — which
is unacceptable. Therefore:

**Signing policy:** RACCOON's release key signs **firmware images and reference
configurations** (which is what integrity protection is for). **Tune authenticity for
third parties** is handled by the credential prompt + audit log + per-device identity, not
by forcing every tune through a key we will never give away. An unsigned tune is always
visibly labelled `UNTRUSTED` in every view, and that label is written into the log header
so a log can never silently misrepresent its own provenance.

This is stated explicitly because it is a design decision with a real trade-off, and a
reviewer will ask.

### 3.2 Verified industry precedent for signing and "untrusted data" warnings

Public evidence is thin; recorded honestly:
- ECUMaster publishes that projects *"can be password protected"* — i.e. obfuscation, not
  signatures. `[VERIFIED: ecumaster.com EMU_PRO Software Guide v226.0]`
- MaxxECU ships an *"integrity check"* on tune files and improved the message for a
  *corrupt* tune. `[VERIFIED: maxxecu.com/mtune changelog]`
- MoTeC T2 states *"Full data encryption from car to server"* for telemetry.
  `[VERIFIED: motec.com.au T2 datasheet]`
- ESP-IDF's **signed data partitions** are the closest public mechanism to a signed
  calibration blob, and they are the one RACCOON will use.
  `[VERIFIED: esp-idf ota API reference]`
- **No public evidence was found of any aftermarket ECU using asymmetric signatures over
  tune files.** → `UNK-REF-006`. RACCOON will be ahead of the market here; that also means
  there is no prior art to copy and the design must stand on its own review.

---

## 4. REVISED ARCHITECTURE — dual MCU

```
┌──────────────────────────────────────────────────────────────────────┐
│  ESP32-S3  (WROOM-1U-N8R2, external antenna, 85 °C)                 │
│  ─ owns: Wi-Fi, BLE, TLS, the signing/verification root, OTA staging, │
│          the ESP32 USB-CDC endpoint, TWAI CAN bridge                 │
│  ─ security: Secure Boot v2 (RSA-3072, ROM-anchored) + Flash         │
│             Encryption (XTS-AES, HW key) + signed data partitions    │
│  ─ power: light-sleep when idle; 355 mA peak budget on the 3.3 V rail│
└───────────────┬──────────────────────────────────────────────────────┘
                │ SPI (preferred, higher bulk throughput) or UART ≤5 Mbaud
                │ framed link: RACCOON protocol + CRC + sequence + length
                │ ⚠ never on the real-time path
┌───────────────▼──────────────────────────────────────────────────────┐
│  STM32H733  (LQFP144)  ← real-time engine control only                │
│  FDCAN1/2/3 (FDCAN1 has TT) · ADC1/2 16-bit + ADC3 12-bit ·         │
│  TIM2/5/23/24 (32-bit GP) + TIM1/8 (16-bit advanced, complementary)  │
│  DTCM 128 KB / ITCM 64 KB+ / AXI-SRAM · 1 MB flash, 128 KB sectors    │
│  crypto accelerator (AES/SHA) → verifies an HMAC/short MAC issued by │
│  the ESP32 after it verified the RSA signature (no RSA in the STM32)  │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.1 The security-root decision (this is the important consequence)

Because the H725 has **no** crypto accelerator and the H733's TrustZone is unconfirmed, the
architecture **does not put the trust root in the STM32**:

1. The **ESP32-S3** holds the RSA-3072 root: eFuse-anchored digest, ROM-verified secure
   boot, hardware flash encryption.
2. The ESP32-S3 verifies a **signed package** (firmware image, configuration package, or
   OTA blob) and then hands the STM32 a **short, time-limited authorisation token**
   (HMAC over the image hash, keyed from a per-device shared secret established at factory
   provisioning).
3. The STM32 verifies the token with its own AES/SHA accelerator — cheap, symmetric, and
   within its capability.
4. **Consequence:** the STM32 cannot be made to accept anything the ESP32 has not
   authorised, and the STM32 needs no RSA implementation. Both parts are doing what they
   are actually good at.
5. **Failure handling:** if the ESP32 is dead, absent, or unpowered, the STM32 refuses to
   activate a new image or accept a new configuration, and continues running the
   **already-activated, already-authorised** configuration. The engine never stops because
   the wireless side failed. (Requires the authorised configuration to be persisted on the
   STM32 at activation time — not merely cached on the ESP32.)

`UNK-SW-003` (exact key provisioning and token format) is now the top open security item.

### 4.2 The "configure a bike, then push it" pipeline (definitive)

```
1.  CREATE VEHICLE          GUI wizard collects engine geometry, crank/cam, harness pin
                            assignment, sensors, fuel, lambda, CAN, modes.  (Data entry
                            only — no compilation anywhere in this product.)
2.  VALIDATE                The SAME validator runs in the GUI and in the ECU
                            (firmware/safety + firmware/calibration). GUI-side validation
                            is a convenience; ECU-side is authoritative.
3.  PACKAGE                 Signed package = header (schema version, hardware profile id,
                            engine profile id, ECU serial target) + payload (calibration
                            image) + per-block CRC + whole-image hash + signature.
4.  DELIVER                 USB-C (direct STM32 or via ESP32), CAN, or Wi-Fi. Idempotent,
                            sequenced, resumable, confirmed. Retries are bounded.
5.  TRANSACT                ECU: STAGE into the inactive slot → CRC → COMMIT → VERIFY →
                            ACTIVATE. Power loss at any step ⇒ the old image runs.
6.  VERIFY + ARM            ECU re-reads the active image, compares the hash, runs the
                            arming gate, and reports ENGINE CONTROL ARMED / NOT ARMED with
                            the reason.  ← §98, and the thing Speeduino/rusEFI cannot do.
```

**Firmware flashing is a separate, explicit operation** (as at Link, Haltech, MaxxECU,
FuelTech, ECUMaster) — never a side effect of configuring a bike.

### 4.3 Configuration is a first-class, versioned object

```
VehicleConfiguration {
    id, name, created, modified, author, notes
    engine_profile      (geometry, cylinders, firing order, cycle, trigger profiles)
    hardware_profile    (the board revision, its pin map, its capabilities)
    wiring              (logical → physical assignment, for THIS bike)
    sensors             (transfer functions, ranges, fault policies)
    strategies          (fuel, lambda, idle, injection, ignition, knock, boost, VVT)
    can_db              (signal database)
    safety              (hard limits)
    calibration_version (the tune that sits on top of this configuration)
}
```

It is a file, it is versioned, it diffs, it merges, it exports, and a project binds one
configuration to one ECU serial. **Two bikes never share a configuration implicitly** — the
ECU refuses a configuration whose `hardware_profile` or schema version does not match.

---

## 5. Revised hardware decisions

### 5.1 Primary MCU: **STM32H733**, LQFP144 (change from H725)

| Reason | Detail |
|---|---|
| **Crypto accelerator present** | Required for the token verification of §4.1. The H725 has none. `[VERIFIED: ST H733 product page + DS13311]` |
| 550 MHz Cortex-M7, DP-FPU | Same class as the H725. `[VERIFIED: DS13311 family docs]` |
| 3 × FDCAN, FDCAN1 with TT-CAN | ✓ |
| 1 MB flash, 128 KB sectors, ECC SEC-DED | A/B at **sector** granularity. `[VERIFIED: DS13311 §3.3.1]` |
| LQFP144 = 97 I/O | Required: LQFP100's 67 I/O cannot host a ~30-channel timer budget plus 16 analog inputs. `[VERIFIED: DS13311 Table 2]` |
| Trade-offs accepted | LDO only (no SMPS); 16-bit ADCs "up to 18 channels" (vs 22 on the H725); 16-bit FMC. |
| 10-year longevity to 01/2036, Active, in volume production | `[VERIFIED: st.com product page]` |

⚠ **FDCAN message RAM is 10 KB shared across all three controllers.** Budget ~1.5–3 KB per
bus. Do not enable three fully-populated FD buses.

**Cost/lead-time warning:** ST eStore shows H725VET6 at $12.18 @1 pc / $7.86 @250–500;
aggregator listings show Future at **52 weeks** and Avnet/EBV at 15–16 weeks. H733 pricing
is `[UNKNOWN]`. **Source early, from an authorised channel only** — gray-market brokers
quote 2–2.5× authorised pricing with no lot traceability, which is unacceptable for a
motorsport ECU. `[VERIFIED: st.com estore + distributor aggregators, 2026-09-29]`

**Not AEC-Q100 qualified** — H725/H723/H733 are graded **Industrial only** (−40…+85 °C
ambient). This is a deliberate qualification decision, recorded. `[VERIFIED: st.com parametric table]`

### 5.2 Cost-down: **STM32G474** — promoted

Reassessment in light of C-03: the G474 has **true dual-bank flash with read-while-write**
(the H725/H733 do not), **hardware ADC oversampling**, **HRTIM at 184 ps**, 5 × 4 MSPS
ADCs, 6 op-amps, 7 comparators, 3 × FDCAN. Its weaknesses are 128 KB total RAM and 170 MHz.
**Decision: keep as the cost-down profile; move calibration to external QSPI flash on this
variant** (it has Quad-SPI), which also solves the RAM pressure. `[VERIFIED: st.com G474 page]`

### 5.3 Power stage: discrete, not a smart-driver IC

Both NXP smart-driver parts are **EOL** (C-09). Requirements for the discrete design, which
must reproduce the useful features:

| Requirement | Why |
|---|---|
| Injector: peak-and-hold, ≥ 4.5 A peak, hold current regulation, hard current limit | §28 |
| Ignition: IGBT gate drive, **flyback clamp**, primary current sense | §30, §33 |
| **Spark-duration feedback** | a primary-side divider (9:1 was the MC33810 reference value) turns "we commanded a spark" into "a spark voltage event was observed" — the measurement the safety case needs |
| **Hardware max-dwell timer** | must survive a halted CPU (§33) |
| Over-temperature shutdown, and a hardware disable path | §49, §103 |
| Current sense on the ignition primary for dwell regulation and over-current | §33 |

Candidate: **MC33814** (Active) is a 2-cylinder small-engine IC with only one injector
driver, so it is unsuitable as the primary stage for 4 cylinders; it is recorded as a
low-cost option for a 1–2 cylinder variant. `[VERIFIED: nxp.com MC33814, 2026-09-29]`

### 5.4 Crank conditioning: keep MAX9926, but confirm lifecycle in writing

ADI publishes no NRND/obsolete banner for the MAX9924–9927 family and names no successor;
automotive temperature range −40…+125 °C; threshold = 1/3 of the previous cycle's peak;
85 ms watchdog threshold recovery; 5 Hz–25 kHz.
`[VERIFIED: MAX9924–MAX9927 datasheet Rev 5]` · lifecycle = `[UNKNOWN — ADI page returned
HTTP 403; obtain written confirmation]`

A modern alternative exists and is credible: MAX9926 **Mode B** disables the internal
adaptive threshold and takes it from an external `EXT` pin driven by a microcontroller PWM,
which lets *our* firmware own the threshold algorithm. Combined with the G474's 6 op-amps /
7 comparators / HRTIM, on-chip conditioning is viable. This is an **open option**, not a
decision: `UNK-HW-003`.

⚠ **Do not confuse VR conditioning with Melexis MLX90513/90514.** Those are *inductive*
(LC-oscillator) interfaces requiring a bespoke Tx/Rx coil and target — **not** a drop-in for
a magnetic pickup. They are frequently mis-cited as a MAX9926 alternative.
`[VERIFIED: melexis.com MLX90513 datasheet, 2026-09-29]`

### 5.5 Wideband: CJ125 is correct, procurement is the risk

Bosch states CJ125 is *"Recommended for new applications"*, supports LSU5.2/4.9/ADV/4.2,
λ = 0.65…∞, SPI interface at 2 Mbaud with a 16-bit frame, LQFP32, −40…+150 °C junction, with
integrated pump-current control, Ri measurement/calibration and sensor-line diagnostics.
`[VERIFIED: bosch-semiconductors.com CJ125 product page + product-info PDF, 2019-02]`

**Procurement reality:** **direct sales only** — not listed through DigiKey/Mouser/element14;
pricing and lead time are `[UNKNOWN]` and require a volume forecast. Plan on **external
wideband controllers (CAN/analog) as the V2 default** and native CJ125 as a
capability-gated upgrade. `[VERIFIED: CJ125 product-info contact list]`

### 5.6 Timing target: re-derived

`[CALCULATED]` — at **12 000 rpm one crank degree = 13.889 µs**. So the ±5 µs target is
**±0.36° of crank**, which is ~0.6 % of a tooth on a 36-tooth wheel. The STM32 timer tick at
275 MHz is 3.64 ns, i.e. the *timer* contributes ~0.03 % of the error budget; the real
error sources are **crank-position accuracy** and **worst-case ISR latency to the compare
load**, not timer resolution.

**Therefore the ±5 µs figure is a realistic but non-trivial HIL acceptance target, and it
must be measured per the method in `HIL_SPEC.md` §3 — not asserted.** `UNK-FW-002`,
`UNK-FW-003` remain open until measured.

---

## 6. Power-supply test standard (corrected)

- **ISO 7637-2:2011** covers electrical transients from the supply; it **excludes** pulses
  4, 5a and 5b. `[VERIFIED: ISO 7637-2:2011 §1 scope]`
- **ISO 16750-2** and **ISO 21848** now cover load dump (pulse 5a/5b) and the other
  environment-related conditions. `[VERIFIED: ISO 7637-2:2011 scope cross-reference]`
- Therefore the ECU test plan must specify **ISO 16750-2** for load dump, cranking
  (pulse 4) and reverse polarity, and **ISO 7637-2:2011** for the remaining electrical
  transients. `UNKNOWN_REGISTER` `UNK-HW-007` updated accordingly.

---

## 7. Updated open items (new, from this verification)

| ID | Question | Blocking? |
|---|---|---|
| `UNK-HW-012` | H733 **TrustZone** availability — explicitly unverified. If absent, the STM32 token store must be protected by RDP/PCROP + an obfuscated layout, and the security claim must be weakened accordingly | Security review |
| `UNK-HW-013` | H733 exact per-package SRAM, ADC channel count on LQFP144, and which `TIMx_CHy` are bonded out on LQFP144 — **must be counted from DS13311 Table 8/9 before schematic capture** | **Yes — schematic** |
| `UNK-HW-014` | H733 pricing and lead time; authorised-channel sourcing plan | **Yes — BOM** |
| `UNK-HW-015` | FDCAN message-RAM budget: 10 KB shared across 3 controllers — is 2 buses + 1 spare viable? | **Yes — CAN design** |
| `UNK-HW-016` | Discrete ignition/injector power stage: IGBT selection, flyback clamp, 9:1 spark-sense divider, hardware max-dwell implementation | **Yes — power stage** |
| `UNK-HW-017` | `MAX9926` lifecycle confirmation from ADI in writing | No |
| `UNK-SW-006` | Token/HMAC scheme between ESP32-S3 and STM32H733, and factory key provisioning | **Yes — security design** |
| `UNK-SW-007` | Wi-Fi association/re-association latency on the target antenna and AP — must be measured; Espressif publishes no figure | No (wireless UX) |
| `UNK-REF-006` | No public evidence of any aftermarket ECU using asymmetric tune signatures. Design must stand on internal review alone | No |

---

## 8. What this document changes in the code

1. `hardware_profiles/raccoon_v2_rev_a` targets **STM32H733 / LQFP144**, with a
   **sector-granular A/B** layout (128 KB sectors) and an explicit `esp32_s3` comms-coprocessor
   declaration.
2. A/B and firmware-update design must assume **sector granularity**, not bank granularity.
3. The security module gains a **token/authorisation interface**; the signature check lives
   on the ESP32 side and is *reported* to the STM32, not performed by the STM32.
4. The vehicle-configuration model becomes a first-class versioned object with a
   **package/flash pipeline**, and the ECU gains a configuration-activation transaction
   distinct from a firmware update.
5. The GUI's tune writer gains the **UNTRUSTED-tune credential prompt** and stamps
   `signature_status` into the log header.
6. The power-stage spec loses MC33810/MC33813 and gains a discrete design with a required
   hardware max-dwell timer and spark-duration feedback.
7. The test plan cites **ISO 16750-2** for load dump/cranking/reverse polarity.
