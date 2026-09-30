# GT650 APPLICATION RESEARCH (PHASE 0)

**Document ID:** GT650-001
**Status:** PHASE 0 — public-information research only. **No physical measurement has been performed.**
**Rule (spec §109):** No older model's information is accepted as proof of the 2023 model.
Every value below carries a source and a verification state. Values that cannot be
established from public sources are recorded in [`UNKNOWN_REGISTER.md`](UNKNOWN_REGISTER.md)
as **blocking or non-blocking measurement items** — they are *not* filled with plausible
guesses.

Verification states:
`PUB-PRIMARY` (manufacturer document) · `PUB-SECONDARY` (dealer/press) · `DERIVED` (computed
from cited values) · `MEASURE` (must be measured on hardware) · `UNKNOWN` (not established).

---

## 1. Target definition

| Item | Value | Source | State |
|---|---|---|---|
| Application | Royal Enfield Continental GT 650 (Interceptor 650 engine family) | Manufacturer | `PUB-PRIMARY` |
| Market | India + international; 2023 model year is the primary target | Manufacturer (2023 press/spec sheets for this model) | `PUB-PRIMARY` |
| Displacement | 647.95 cc (marketing: 648 cc) | RE Technical Specifications (UK/India PDFs) | `PUB-PRIMARY` |
| Configuration | Parallel twin, 4-stroke, SOHC, air-cooled, oil-cooled (wet sump, pump-driven oil delivery) | RE Technical Specifications | `PUB-PRIMARY` |
| Bore | 78 mm | RE Technical Specifications | `PUB-PRIMARY` |
| Stroke | 67.8 mm | RE Technical Specifications | `PUB-PRIMARY` |
| Compression ratio | 9.5 : 1 | RE Technical Specifications (also Wikipedia, consistent) | `PUB-PRIMARY` |
| Swept-volume check | π/4 × 78² × 67.8 mm = **648.0 cm³** | computed | `DERIVED` — confirms the published 647.95 cc |
| Valves | 4 per cylinder (2 in, 2 ex) | RE Spec Sheet | `PUB-PRIMARY` |
| Max power (OEM) | 34.9 kW @ 7250 rpm (RE spec) / 47.65 PS @ 7150 rpm (Wikipedia) — these are the same engine quoted on different bases; treat as **OEM claim, not RACCOON-measured** | RE Spec Sheet + Wikipedia | `PUB-PRIMARY` / claim only |
| Max torque (OEM) | 52.3 N·m @ 5150 rpm (RE spec) | RE Spec Sheet | `PUB-PRIMARY` / claim only |
| Idle | 1200 ± 100 rpm | RE Spec Sheet | `PUB-PRIMARY` |
| Fuel system | Fuel injection | RE Spec Sheet | `PUB-PRIMARY` |
| Spark | BOSCH UR5CC plug, gap 0.7–0.8 mm | RE Spec Sheet | `PUB-PRIMARY` |
| OEM ignition advance | 11.25° BTDC (published idle/cruise value) | RE Spec Sheet | `PUB-PRIMARY` (a published calibration value, *not* a tuning target) |
| Gearbox | 6-speed constant mesh | RE Spec Sheet | `PUB-PRIMARY` |
| Final drive | 2.533 : 1 (5/8" chain) | RE Spec Sheet | `PUB-PRIMARY` |
| Primary ratio | 2.051 : 1 | RE Spec Sheet | `PUB-PRIMARY` |
| Gear ratios | 1st 2.615, 2nd 1.813, 3rd 1.429, 4th 1.190, 5th 1.040, 6th 0.962 | RE Spec Sheet | `PUB-PRIMARY` |
| Clutch | Wet multi-plate, slip & assist (Wikipedia) | RE manual + Wikipedia | `PUB-PRIMARY` |
| Electrical | 12 V DC, 12 Ah VRLA battery; alternator 156 W @ 1100 rpm | RE Owners Manual (UK Euro-4) | `PUB-PRIMARY` |
| Fuel | Unleaded / E10 or less | RE Spec Sheet | `PUB-PRIMARY` |
| Engine type note | Block engine: crankshaft and gearbox in one housing; intake-manifold injection | Wikipedia | `PUB-SECONDARY` |

**Consequence for RACCOON architecture:** this is a 2-cylinder, 8-valve, SOHC,
naturally-aspirated, fuel-injected motorcycle. A **complete ECU is not required for engine
control** — the OEM ECU's engine functions are replaced by RACCOON. This is the primary
justification for the standalone requirement (spec §5).

---

## 2. The critical unknowns (why they gate the design)

The engine geometry is well published. **The electrical interface is not.** The following
are the items that determine whether a wiring harness, a trigger strategy, and a first
calibration can be built at all.

| ID | Item | Why it matters | State | Register |
|---|---|---|---|---|
| G-01 | **Crank trigger source**: does the crank wheel have a missing tooth? How many teeth? Does the cam wheel have teeth, and how many? Is the sensor VR or Hall? | Determines the entire trigger subsystem and the safe start strategy | `UNKNOWN` | `UNK-GT-001` |
| G-02 | **Cam signal**: is a cam sensor fitted at all, single or dual? Its tooth count and phase relative to TDC? | Without cam phase, sequential injection/ignition is not physically valid | `UNKNOWN` | `UNK-GT-002` |
| G-03 | **Firing order and cylinder geometry** (which cylinder is 1, TDC offset between cylinders) | Ignition and injection phasing | Parallel twin: order is conventionally 1-2 with 180° crank throw, so firing events are 360° apart within a 720° cycle. `INFERRED`, must be confirmed by strobe + cam observation | `UNK-GT-003` |
| G-04 | **Injector type and impedance**; flow rate (cc/s at a stated pressure); connector and pin | Dead time, minimum pulse, duty ceiling, whether peak-and-hold hardware is required | `UNKNOWN` | `UNK-GT-004` |
| G-05 | **Fuel pressure** (idle, cruise, WOT) and whether the returnless system holds pressure when the pump is off | Dead-time compensation, minimum-pressure protection, limp mode | `UNKNOWN` | `UNK-GT-005` |
| G-06 | **Ignition coil architecture**: single-coil + external driver? one coil per cylinder? coil primary resistance, turns ratio, saturation dwell | Dwell model, driver topology, CDI compatibility (**assumed incompatible** — spec §30) | `UNKNOWN` | `UNK-GT-006` |
| G-07 | **TPS sensor(s)**: 1 or 2? 0–5 V or 0.5–4.5 V with 5 V reference? cold/warm behaviour? | Load strategy selection, DBW option | `UNKNOWN` | `UNK-GT-007` |
| G-08 | **MAP sensor present?** part number, range, whether 2-bar/3-bar/4-bar, resolution | Speed-density viability | `UNKNOWN` | `UNK-GT-008` |
| G-09 | **IAT / ECT sensor**: NTC curve (Ω at 25 °C and β), or linear | Correctness of cold-start enrichment and ignition corrections | `UNKNOWN` | `UNK-GT-009` |
| G-10 | **Lambda**: narrowband fitting count and location? Is a wideband socket present? Is the sensor a LSU or a Bosch 4.9? | Closed-loop strategy, per-cylinder feasibility, autotune validity | `UNKNOWN` | `UNK-GT-010` |
| G-11 | **Idle control actuator**: present? Stepper IAC / PWM IAC / none (idle by ignition+air bypass)? | Idle strategy (§39) | `UNKNOWN` | `UNK-GT-011` |
| G-12 | **Fuel pump control**: PWM, on/off, or a pressure sensor + returnless logic | Pump duty limits, low-pressure protection | `UNKNOWN` | `UNK-GT-012` |
| G-13 | **Vehicle/CAN**: does the bike have a CAN bus (e.g. to the cluster/ABS)? Bitrate, IDs | Vehicle functions, wheel-speed source, gear source, dash | `UNKNOWN` | `UNK-GT-013` |
| G-14 | **Wheel speed source**: is there an ABS wheel-speed sensor (and thus a usable output)? Or must we fit one? | Traction control and speed | `UNKNOWN` | `UNK-GT-014` |
| G-15 | **Gear position source**: no gear sensor on most parallel twins; must be derived from rpm/speed/clutch or a fitted sensor | Torque management, TC, pit limiter, launch | `UNKNOWN` | `UNK-GT-015` |
| G-16 | **Clutch switch / lever switch** present? | Flat shift, launch, auto-blip (§43–44) | `UNKNOWN` | `UNK-GT-016` |
| G-17 | **OBD/diagnostics interface**: is the OEM ECU OBD-II/ISO-KWI compatible? Can we run a bidirectional test in a safe state? | Post-install validation, free-air check of sensor positions | `UNKNOWN` | `UNK-GT-017` |
| G-18 | **Oil pressure / oil temperature sender** present (0.5 bar switch or analogue)? | Engine protection §49 | `UNKNOWN` | `UNK-GT-018` |

### What is *not* needed for a first working standalone ECU
Per §5, RACCOON does not need the OEM ECU for engine control. The OEM ECU may stay
connected for unrelated vehicle functions. Therefore G-13/G-17 are **not blocking** for
Phase 2–6 firmware; they are blocking for full feature enablement (traction, dash,
diagnostics).

### The minimum measurement set to unblock calibration
1. Trigger wheel photograph + tooth count + a scope capture of raw CKP at cranking and at
   6000 rpm.
2. Cam signal capture + observed phase at TDC.
3. Strobe verification of firing order and TDC.
4. Injector part number and flow curve; fuel pressure at three points.
5. Coil part number, primary resistance, and measured secondary spark.
6. Sensor part numbers for TPS, MAP, ECT, IAT, O2.
→ recorded as the **GT650 Bench Characterisation Plan** in `TEST_PLAN.md` §GT650.

---

## 3. Engine-derived constants (computable now, from published geometry)

These are `DERIVED` and are *architecture* inputs, not calibration:

| Constant | Value | Derivation |
|---|---|---|
| Displacement | 648.0 cm³ | π/4 × 78² × 67.8 |
| Stroke volume per cylinder | 162.0 cm³ | 648 / 4 (4-stroke) |
| Crank throw (radius) | 33.9 mm | stroke / 2 |
| Rod ratio | **UNKNOWN** | not published → `UNK-GT-019` |
| Firing interval (parallel twin, 180° crank) | 360° crank per fire; 4 fires / 720° cycle | `INFERRED` — confirm with `UNK-GT-003` |
| Cycle | 720° | 4-stroke, `PUB-PRIMARY` |
| Volumetric displacement per revolution (2-cyl, 4-stroke) | 324.0 cm³/rev | 648/2 |
| Compression ratio | 9.5 : 1 | `PUB-PRIMARY` |

Note the **derivation of firing interval is the single most safety-critical GT650 fact**:
if the firing order or cylinder phasing is wrong, a sequential system can inject into a
cylinder on its exhaust stroke and cause severe damage. RACCOON therefore implements the
**arming safety gate** (spec §98) which *refuses to arm* until TDC and firing order have
been confirmed by the bench procedure, and the TDC tool in the GUI.

---

## 4. Firing-order and phase determination method (to be executed in Phase 12)

1. Disable fuel and spark, crank by starter only.
2. Observe cam position at the moment the crank's reference feature aligns.
3. Confirm cylinder #1 TDC with a piston-position rod or by cam-lobe observation.
4. Strobe the timing light at 1500 rpm against the **known** trigger angle and record
   whether the mark advances with or against crank direction (per engine: engines rotate
   forwards; a backwards-reading strobe indicates the setup is inverted — a classic
   mis-synchronisation failure).
5. Record the cam-to-crank phase offset; this becomes `cam_phase_offset_deg` in the
   engine profile, with source `MEASURE`.

**Anti-pattern explicitly forbidden (spec §12, §102):** never fabricate a phase source.
If no cam signal exists, the configuration must either (a) add a cam sensor, or (b) run a
crank-only strategy that physically cannot support sequential operation — and the GUI
must refuse a sequential configuration in that case (§97, §98).

---

## 5. Capability expectation for the GT650 profile

| Feature | GT650 hardware expectation | Consequence |
|---|---|---|
| Sequential injection | **Unknown** — depends on G-01/G-02. If no cam, sequential is unsafe. | Profile ships in the safest configuration the measured hardware allows. |
| Sequential ignition | Same as above | |
| Wasted spark ignition | Plausible fallback (requires 2 coils or a paired driver) | |
| Closed loop (narrowband) | Depends on G-10 | |
| Wideband | External CJ125+LSU4.9 on a daughterboard, or a serial/CAN controller | capability-gated |
| Knock | Requires a knock sensor to be **fitted** (motorcycle engines may not have one) | Do not claim knock without hardware |
| Traction control | Requires a wheel-speed source (G-14) | do not claim without hardware |
| DBW | Not fitted on a cable-throttle bike | architecture only, capability-gated |
| Boost | Not applicable to stock NA engine | architecture only |
| VVT | Not applicable (fixed cam, SOHC) | architecture only |
| Launch / flat shift | Feasible with a **switch or strain-gauge input added** | shift strategy needs a request source |
| Logging | Internal (mandatory) | |

**Explicit statement:** the GT650 profile will *not* advertise knock, traction, DBW, boost,
VVT, or wideband unless the required hardware is physically present in the vehicle and
declared in the hardware profile (spec §21, §34, §38, §94, §100).
