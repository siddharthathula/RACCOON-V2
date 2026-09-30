# Calibration Reference

**Generated from `configs/schema/*.yaml`. DO NOT EDIT.**

Schema version 1 - image size 4716 bytes - 537 items.

All values are in **engineering units**. Provenance tags: SOURCE-DERIVED / CALCULATED / MEASURED / SIMULATED / HIL-VERIFIED / BENCH-MEASURED / ENGINE-TESTED / DYNO-MEASURED / VEHICLE-VALIDATED / UNVERIFIED

## `engine`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0100` | `cylinder_count` | u8 | | 2 | 1 | 4 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0101` | `bore_mm` | f32 | mm | 78 | 20 | 150 | ENGINEER | HIGH | SOURCE-DERIVED GT650-001 sec.1 (RE technical specification) |
| `0x0102` | `stroke_mm` | f32 | mm | 67.8 | 10 | 200 | ENGINEER | HIGH | SOURCE-DERIVED GT650-001 sec.1 |
| `0x0103` | `compression_ratio` | f32 | :1 | 9.5 | 4 | 20 | ENGINEER | HIGH | SOURCE-DERIVED GT650-001 sec.1 |
| `0x0104` | `rod_ratio` | f32 | :1 | 1.45 | 0.8 | 2.5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0105` | `cycle_teeth` | enum | | four_stroke_720 | 0 | 1 | ENGINEER | CRITICAL | SOURCE-DERIVED |
| `0x0106` | `crank_layout` | enum | | parallel_180 | 0 | 5 | ENGINEER | CRITICAL | SOURCE-DERIVED GT650-001 sec.1 (parallel twin) |
| `0x0107` | `firing_order_cyl0` | u8 | | 0 | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0108` | `firing_order_cyl1` | u8 | | 1 | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0109` | `firing_order_cyl2` | u8 | | 2 | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x010A` | `firing_order_cyl3` | u8 | | 3 | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x010B` | `custom_firing_angle_0` | f32 | deg | 0 | 0 | 720 | ENGINEER | HIGH | UNVERIFIED |
| `0x010C` | `custom_firing_angle_1` | f32 | deg | 180 | 0 | 720 | ENGINEER | HIGH | UNVERIFIED |
| `0x010D` | `custom_firing_angle_2` | f32 | deg | 360 | 0 | 720 | ENGINEER | HIGH | UNVERIFIED |
| `0x010E` | `custom_firing_angle_3` | f32 | deg | 540 | 0 | 720 | ENGINEER | HIGH | UNVERIFIED |
| `0x010F` | `cam_phase_offset_deg` | f32 | deg | 0 | -180 | 180 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0110` | `max_rpm` | u16 | rpm | 9000 | 1000 | 2e+04 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0111` | `idle_rpm_target` | u16 | rpm | 1200 | 400 | 4000 | TUNER | MEDIUM | SOURCE-DERIVED GT650-001 sec.1 (idle 1200 +/- 100 rpm) |
| `0x0112` | `fuel_type` | enum | | gasoline | 0 | 3 | TUNER | MEDIUM | SOURCE-DERIVED GT650-001 sec.1 (unleaded / E10 or less) |
| `0x0113` | `fuel_ethanol_fraction` | f32 | frac | 0 | 0 | 1 | TUNER | MEDIUM | SOURCE-DERIVED |
| `0x0114` | `fuel_stoich_afr_gasoline` | f32 | AFR | 14.7 | 10 | 20 | TUNER | HIGH | SOURCE-DERIVED |
| `0x0115` | `fuel_stoich_afr_ethanol` | f32 | AFR | 9 | 6 | 15 | TUNER | HIGH | SOURCE-DERIVED |
| `0x0116` | `fuel_density_gasoline` | f32 | g/L | 745 | 600 | 900 | TUNER | MEDIUM | SOURCE-DERIVED |
| `0x0117` | `fuel_density_ethanol` | f32 | g/L | 789 | 600 | 900 | TUNER | MEDIUM | SOURCE-DERIVED |
| `0x0118` | `air_density_sea_level` | f32 | g/L | 1.225 | 0.3 | 2 | READ_ONLY | MEDIUM | SOURCE-DERIVED |
| `0x011A` | `displacement_cc` | f32 | cc | 0 | 0 | 2e+04 | ENGINEER | MEDIUM | UNVERIFIED |

**Notes**

- `cylinder_count` - Number of cylinders. 1..4 is the supported product range; the engine core contains no per-engine code, only this number and the geometry below.
- `rod_ratio` - Connecting rod length / crank radius. Not published for the GT650; it only affects the pumping-loss and volumetric-efficiency model, so a conservative value is safe. Register UNK-GT-019.
- `cycle_teeth` - Engine cycle. 720 degrees is the only four-stroke timing model implemented.
- `crank_layout` - Crank arrangement. Determines the firing angles used by the scheduler.
- `firing_order_cyl0` - Which physical cylinder fires first. Wrong values inject into an exhaust stroke, so this is gated by the arming check and verified on the bench. Register UNK-GT-003.
- `custom_firing_angle_0` - Firing angle of cylinder 0 after its TDC, for crank_layout = custom.
- `cam_phase_offset_deg` - Crank angle of the cam reference edge relative to cylinder 1 TDC. Without a measured value the configuration must not allow sequential operation. Register UNK-GT-002.
- `max_rpm` - Design maximum. The hard limiter uses the safety copy, not this value; this is used for table axis scaling and for plausibility rejection.
- `fuel_ethanol_fraction` - Ethanol fraction of the current fuel, from a flex-fuel sensor or set by hand. Drives stoich AFR, density, cold start, ignition blend and lambda target.
- `fuel_stoich_afr_gasoline` - Stoich air/fuel ratio for pump gasoline (14.7:1).
- `fuel_stoich_afr_ethanol` - Stoich AFR for pure ethanol (about 9.0:1).
- `air_density_sea_level` - Dry air density at 15 C, 101.325 kPa. Reference for the barometric correction.
- `displacement_cc` - Optional: a stated total displacement, cross-checked against bore x stroke at configuration load. A disagreement beyond 2 percent refuses the configuration rather than being quietly preferred. Leave at 0 to derive it purely.

## `trigger`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0200` | `profile_crank` | enum | | custom | 0 | 13 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0201` | `profile_cam` | enum | | none | 0 | 4 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0202` | `teeth` | u8 | | 36 | 2 | 120 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0203` | `missing_teeth` | u8 | | 1 | 0 | 12 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0204` | `teeth_per_rev` | u8 | | 1 | 1 | 2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0205` | `tooth1_angle_atdc` | f32 | deg | 0 | 0 | 360 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0206` | `trigger_edge_polarity` | bits[2] | | 0 | 0 | 3 | ENGINEER | HIGH | UNVERIFIED |
| `0x0207` | `input_filter_level` | u8 | | 1 | 0 | 3 | TUNER | HIGH | UNVERIFIED |
| `0x0208` | `sync_skip_revolutions` | u8 | | 0 | 0 | 3 | TUNER | MEDIUM | UNVERIFIED |
| `0x0209` | `min_period_us` | u32 | us | 40 | 20 | 1e+05 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x020A` | `max_period_us` | u32 | us | 2e+06 | 1e+04 | 1e+07 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x020B` | `overspeed_period_us` | u32 | us | 80 | 20 | 5000 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x020C` | `stall_timeout_ms` | u16 | ms | 500 | 50 | 5000 | TUNER | HIGH | UNVERIFIED |
| `0x020D` | `prediction_enabled` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x020E` | `max_accel_rpm_per_s` | u16 | rpm/s | 2e+04 | 100 | 6e+04 | ENGINEER | HIGH | UNVERIFIED |
| `0x020F` | `cam_teeth` | u8 | teeth | 0 | 0 | 8 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0210` | `cam_missing_teeth` | u8 | teeth | 0 | 0 | 2 | ENGINEER | CRITICAL | UNVERIFIED |

**Notes**

- `profile_crank` - Crank trigger wheel profile. The generic profiles cover the common aftermarket wheels; anything else is defined by the explicit profile items below. Register UNK-GT-001.
- `teeth` - Teeth per revolution on this wheel (counted as if none were missing).
- `missing_teeth` - Size of the contiguous missing-tooth gap, in teeth.
- `teeth_per_rev` - 1 for a crank wheel, 2 for a half-speed cam wheel.
- `tooth1_angle_atdc` - Crank angle after TDC at which tooth 1 passes the sensor. This is the single most consequential number in the product; it must be established with a timing light, not guessed.
- `input_filter_level` - Software input filter, 0 off .. 3 aggressive. Default is deliberately mild: a rejected real tooth desynchronises phase, which is worse than accepting one noise edge. Rejection count is a logged channel.
- `sync_skip_revolutions` - Revolutions of confirmation required before sync is declared.
- `min_period_us` - Shortest plausible tooth period. Edges faster than this are rejected, which is what makes an impossible period and overspeed detectable. Register UNK-FW-001.
- `max_period_us` - Longest plausible period before a tooth timeout and SYNC_LOST.
- `overspeed_period_us` - Below this period the decoder declares a hard fault and inhibits output.
- `prediction_enabled` - Predict the rate at the target angle from the measured acceleration, bounded. Disabling it makes timing exact only at constant speed and is for bench work.
- `max_accel_rpm_per_s` - Acceleration bound for the predictor. Beyond it, prediction is disabled for the window and a diagnostic is counted. No unbounded extrapolation is permitted.
- `cam_teeth` - Cam wheel tooth count, used when profile_cam is 'custom'. The named profiles carry their own count and ignore this item, because a 4-tooth half-speed wheel decoded as 3 teeth produces a cam phase that is wrong by a quarter of a cam revolution - and a wrong cam phase under sequential injection is a cylinder fired at the wrong time, which is not a starting problem, it is a broken-engine problem.
- `cam_missing_teeth` - Gap teeth on a custom cam wheel, counted from tooth 1. A cam wheel with no gap has no phase reference, so the decoder cannot tell one cam revolution from the next and sequential injection is refused rather than guessed.

## `sensors`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0300` | `tps1_slope` | f32 | %/V | 20 | 0.1 | 200 | ENGINEER | HIGH | UNVERIFIED |
| `0x0301` | `tps1_offset` | f32 | % | 0 | -20 | 20 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0302` | `tps1_min` | f32 | % | -2 | -10 | 10 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0303` | `tps1_max` | f32 | % | 102 | 90 | 110 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0304` | `tps1_rate_limit` | f32 | %/s | 200 | 10 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (TPS 200 %/s) |
| `0x0305` | `tps1_freshness_ms` | u16 | ms | 20 | 1 | 1000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (TPS 20 ms) |
| `0x0306` | `tps1_transfer` | enum | | ratiometric | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0307` | `tps1_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0308` | `map_slope` | f32 | kPa/V | 200 | 1 | 1000 | ENGINEER | HIGH | UNVERIFIED |
| `0x0309` | `map_offset` | f32 | kPa | 0 | -100 | 100 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x030A` | `map_min` | f32 | kPa | 0 | -50 | 100 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x030B` | `map_max` | f32 | kPa | 300 | 50 | 500 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x030C` | `map_rate_limit` | f32 | kPa/s | 500 | 5 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (MAP 500 kPa/s) |
| `0x030D` | `map_freshness_ms` | u16 | ms | 20 | 1 | 1000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (MAP 20 ms) |
| `0x030E` | `map_transfer` | enum | | ratiometric | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x030F` | `map_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0310` | `maf_slope` | f32 | g/s/V | 10 | 0.1 | 500 | ENGINEER | HIGH | UNVERIFIED |
| `0x0311` | `maf_offset` | f32 | g/s | 0 | -20 | 20 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0312` | `maf_min` | f32 | g/s | 0 | -5 | 10 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0313` | `maf_max` | f32 | g/s | 200 | 10 | 1000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0314` | `maf_rate_limit` | f32 | g/s/s | 500 | 5 | 1e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0315` | `maf_freshness_ms` | u16 | ms | 20 | 1 | 1000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0316` | `maf_transfer` | enum | | ratiometric | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0317` | `maf_fault_behaviour` | enum | | mark_invalid_use_alternate_strategy | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0318` | `ect_slope` | f32 | degC/V | -50 | -500 | 500 | ENGINEER | HIGH | UNVERIFIED |
| `0x0319` | `ect_offset` | f32 | degC | 25 | -100 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x031A` | `ect_min` | f32 | degC | -40 | -100 | 0 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x031B` | `ect_max` | f32 | degC | 150 | 60 | 250 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x031C` | `ect_rate_limit` | f32 | degC/s | 20 | 0.5 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (ECT 20 degC/s) |
| `0x031D` | `ect_freshness_ms` | u16 | ms | 2000 | 1 | 2e+04 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (ECT 2 s) |
| `0x031E` | `ect_transfer` | enum | | ntc_table | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x031F` | `ect_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0320` | `iat_slope` | f32 | degC/V | -50 | -500 | 500 | ENGINEER | HIGH | UNVERIFIED |
| `0x0321` | `iat_offset` | f32 | degC | 25 | -100 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0322` | `iat_min` | f32 | degC | -40 | -100 | 0 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0323` | `iat_max` | f32 | degC | 150 | 60 | 250 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0324` | `iat_rate_limit` | f32 | degC/s | 20 | 0.5 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (IAT 20 degC/s) |
| `0x0325` | `iat_freshness_ms` | u16 | ms | 2000 | 1 | 2e+04 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (IAT 2 s) |
| `0x0326` | `iat_transfer` | enum | | ntc_table | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0327` | `iat_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0328` | `oil_pressure_slope` | f32 | bar/V | 2 | 0.01 | 20 | ENGINEER | HIGH | UNVERIFIED |
| `0x0329` | `oil_pressure_offset` | f32 | bar | 0 | -5 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x032A` | `oil_pressure_min` | f32 | bar | 0 | -5 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x032B` | `oil_pressure_max` | f32 | bar | 10 | 1 | 100 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x032C` | `oil_pressure_rate_limit` | f32 | bar/s | 10 | 0.5 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (oil pressure 10 bar/s) |
| `0x032D` | `oil_pressure_freshness_ms` | u16 | ms | 100 | 1 | 2000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (oil pressure 100 ms) |
| `0x032E` | `oil_pressure_transfer` | enum | | linear | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x032F` | `oil_pressure_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0330` | `oil_temp_slope` | f32 | degC/V | -50 | -500 | 500 | ENGINEER | HIGH | UNVERIFIED |
| `0x0331` | `oil_temp_offset` | f32 | degC | 25 | -100 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0332` | `oil_temp_min` | f32 | degC | -40 | -100 | 0 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0333` | `oil_temp_max` | f32 | degC | 150 | 60 | 250 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0334` | `oil_temp_rate_limit` | f32 | degC/s | 20 | 0.5 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (oil temperature 20 degC/s) |
| `0x0335` | `oil_temp_freshness_ms` | u16 | ms | 2000 | 1 | 2e+04 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (oil temperature 2 s) |
| `0x0336` | `oil_temp_transfer` | enum | | ntc_table | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0337` | `oil_temp_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0338` | `fuel_pressure_slope` | f32 | bar/V | 2 | 0.01 | 20 | ENGINEER | HIGH | UNVERIFIED |
| `0x0339` | `fuel_pressure_offset` | f32 | bar | 0 | -5 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x033A` | `fuel_pressure_min` | f32 | bar | 0 | -5 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x033B` | `fuel_pressure_max` | f32 | bar | 10 | 1 | 100 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x033C` | `fuel_pressure_rate_limit` | f32 | bar/s | 10 | 0.5 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (fuel pressure 10 bar/s) |
| `0x033D` | `fuel_pressure_freshness_ms` | u16 | ms | 100 | 1 | 2000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (fuel pressure 100 ms) |
| `0x033E` | `fuel_pressure_transfer` | enum | | linear | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x033F` | `fuel_pressure_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0340` | `egt_slope` | f32 | degC/V | 167 | 1 | 2000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0341` | `egt_offset` | f32 | degC | 0 | -100 | 500 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0342` | `egt_min` | f32 | degC | 0 | -100 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0343` | `egt_max` | f32 | degC | 1000 | 100 | 2000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0344` | `egt_rate_limit` | f32 | degC/s | 200 | 5 | 2000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0345` | `egt_freshness_ms` | u16 | ms | 500 | 1 | 1e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0346` | `egt_transfer` | enum | | linear | 0 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0347` | `egt_fault_behaviour` | enum | | substitute_fallback | 0 | 6 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0348` | `ethanol_slope` | f32 | frac/V | 0.2 | 0.01 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0349` | `ethanol_offset` | f32 | frac | 0 | -0.5 | 0.5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034A` | `ethanol_min` | f32 | frac | 0 | -0.2 | 0.5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034B` | `ethanol_max` | f32 | frac | 1 | 0.5 | 1.2 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034C` | `ethanol_rate_limit` | f32 | frac/s | 0.1 | 0.001 | 2 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034D` | `ethanol_freshness_ms` | u16 | ms | 1000 | 1 | 2e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034E` | `ethanol_transfer` | enum | | ratiometric | 0 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x034F` | `ethanol_fault_behaviour` | enum | | hold_last_valid_timed | 0 | 6 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0350` | `battery_slope` | f32 | V/V | 3 | 1 | 20 | ENGINEER | HIGH | UNVERIFIED |
| `0x0351` | `battery_offset` | f32 | V | 0 | -2 | 2 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0352` | `battery_min` | f32 | V | 0 | -5 | 10 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0353` | `battery_max` | f32 | V | 18 | 12 | 40 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0354` | `battery_rate_limit` | f32 | V/s | 20 | 1 | 200 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (battery 20 V/s) |
| `0x0355` | `battery_freshness_ms` | u16 | ms | 1000 | 1 | 2e+04 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (battery 1 s) |
| `0x0356` | `battery_transfer` | enum | | linear | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0357` | `battery_fault_behaviour` | enum | | enter_limp_mode | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0358` | `ref5v_slope` | f32 | V/V | 1 | 0.5 | 2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0359` | `ref5v_offset` | f32 | V | 0 | -1 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035A` | `ref5v_min` | f32 | V | 4.6 | 3 | 5.2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035B` | `ref5v_max` | f32 | V | 5.4 | 5 | 6 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035C` | `ref5v_rate_limit` | f32 | V/s | 5 | 0.1 | 100 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035D` | `ref5v_freshness_ms` | u16 | ms | 1000 | 1 | 2e+04 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035E` | `ref5v_transfer` | enum | | linear | 0 | 5 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x035F` | `ref5v_fault_behaviour` | enum | | invalidate_dependents | 0 | 6 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0360` | `lambda1_slope` | f32 | lambda/V | 0.25 | 0.01 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0361` | `lambda1_offset` | f32 | lambda | 0 | -1 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0362` | `lambda1_min` | f32 | lambda | 0.4 | 0.2 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0363` | `lambda1_max` | f32 | lambda | 2 | 1 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0364` | `lambda1_rate_limit` | f32 | lambda/s | 5 | 0.1 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0365` | `lambda1_freshness_ms` | u16 | ms | 40 | 1 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (lambda 40 ms in closed loop) |
| `0x0366` | `lambda1_transfer` | enum | | linear | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0367` | `lambda1_fault_behaviour` | enum | | mark_invalid_use_alternate_strategy | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0368` | `lambda2_slope` | f32 | lambda/V | 0.25 | 0.01 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0369` | `lambda2_offset` | f32 | lambda | 0 | -1 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x036A` | `lambda2_min` | f32 | lambda | 0.4 | 0.2 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x036B` | `lambda2_max` | f32 | lambda | 2 | 1 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x036C` | `lambda2_rate_limit` | f32 | lambda/s | 5 | 0.1 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x036D` | `lambda2_freshness_ms` | u16 | ms | 40 | 1 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (lambda 40 ms in closed loop) |
| `0x036E` | `lambda2_transfer` | enum | | none | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x036F` | `lambda2_fault_behaviour` | enum | | mark_invalid_use_alternate_strategy | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0370` | `lambda3_slope` | f32 | lambda/V | 0.25 | 0.01 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0371` | `lambda3_offset` | f32 | lambda | 0 | -1 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0372` | `lambda3_min` | f32 | lambda | 0.4 | 0.2 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0373` | `lambda3_max` | f32 | lambda | 2 | 1 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0374` | `lambda3_rate_limit` | f32 | lambda/s | 5 | 0.1 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0375` | `lambda3_freshness_ms` | u16 | ms | 40 | 1 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (lambda 40 ms in closed loop) |
| `0x0376` | `lambda3_transfer` | enum | | none | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0377` | `lambda3_fault_behaviour` | enum | | mark_invalid_use_alternate_strategy | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0378` | `lambda4_slope` | f32 | lambda/V | 0.25 | 0.01 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x0379` | `lambda4_offset` | f32 | lambda | 0 | -1 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x037A` | `lambda4_min` | f32 | lambda | 0.4 | 0.2 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x037B` | `lambda4_max` | f32 | lambda | 2 | 1 | 5 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x037C` | `lambda4_rate_limit` | f32 | lambda/s | 5 | 0.1 | 200 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x037D` | `lambda4_freshness_ms` | u16 | ms | 40 | 1 | 5000 | ENGINEER | MEDIUM | SOURCE-DERIVED SENS-001 sec.7 (lambda 40 ms in closed loop) |
| `0x037E` | `lambda4_transfer` | enum | | none | 0 | 5 | ENGINEER | HIGH | UNVERIFIED |
| `0x037F` | `lambda4_fault_behaviour` | enum | | mark_invalid_use_alternate_strategy | 0 | 6 | ENGINEER | HIGH | UNVERIFIED |
| `0x0380` | `sensor_filter_cutoff_hz` | f32 | Hz | 250 | 1 | 5000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0381` | `sensor_dual_check_enabled` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0382` | `sensor_dual_mismatch_max` | f32 | % | 5 | 0.1 | 50 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0383` | `sensor_dual_policy` | enum | | invalidate_both | 0 | 3 | ENGINEER | HIGH | UNVERIFIED |
| `0x0384` | `sensor_frozen_window_ms` | u16 | ms | 2000 | 100 | 6e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0385` | `sensor_reference_5v_enabled` | bool | | 1 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0386` | `sensor_reference_5v_fault_action` | enum | | invalidate_5v_sensors | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |

**Notes**

- `tps1_slope` - Throttle opening per volt at the sensor pins. 20 %/V is a 0..5 V sensor; 25 %/V is the 0.5..4.5 V form, which leaves a live diagnostic band at each stop. Register UNK-GT-007 - the fitted sensor is not yet known.
- `tps1_min` - Lower plausibility bound, deliberately a little below the closed stop so a real idle position is not rejected as RANGE on every cycle.
- `tps1_max` - Upper plausibility bound, the throttle stop plus margin.
- `tps1_rate_limit` - Open-loop rate limiter, not a smoothing filter: RATE_LIMIT is a reported flag so a genuine fast transient stays distinguishable from a limited one (test S13).
- `tps1_transfer` - Transfer-function family for this channel (SENS-001 sec.3). ratiometric divides the code by the ADC full scale and scales by the *measured* 5 V reference, so a drifting sensor rail cannot quietly rescale the throttle.
- `tps1_fault_behaviour` - What the control loop does when this channel is invalid. The choice is a deliberate calibration per the fail-operational / fail-safe split in SC-001 sec.4, never an accidental default.
- `map_slope` - Pressure per volt. 200 kPa/V spans 0..1000 kPa absolute over 0..5 V, which covers both a 2-bar and a 3-bar gauge. Register UNK-GT-008 - the fitted sensor is unknown, and a 4-bar part needs a different slope.
- `map_min` - Lower plausibility bound in absolute terms. A negative band is admitted so a gauge briefly below its zero on a cold start is not a fault.
- `map_max` - Upper plausibility bound. Must not be set below the boost target.
- `map_rate_limit` - Shared with the transient model. A value clipped here is otherwise indistinguishable from a real pressure event, so the limiter is always flagged.
- `map_fault_behaviour` - Speed density has a declared degraded path, so the default is the limp: with MAP invalid the load estimator falls to the alternate strategy and records the substitution in the correction vector (FUEL-001 sec.2).
- `maf_slope` - Air mass flow per volt at the pins. Only relevant when the MAF fuel strategy is selected; a motorcycle normally has no MAF, and an absent sensor is declared by transfer = none rather than by leaving a plausible number behind.
- `maf_max` - Upper plausibility bound; must exceed idle flow with a margin.
- `maf_fault_behaviour` - MAF is the only absolute air measurement, so losing it changes the *nature* of the calculation; the declared behaviour is to switch load strategy, not to continue with a held value.
- `ect_slope` - Linearised front-end gain of the coolant thermistor. Negative, because an NTC on a pull-up reads lower as it heats. With transfer = ntc_table the Steinhart-Hart coefficients from the sensor profile take precedence and this pair is the linearisation seed. Register UNK-GT-009.
- `ect_min` - Lower plausibility bound. It must not be set so high that a genuinely cold engine is reported RANGE and loses cold-start enrichment.
- `ect_fault_behaviour` - A coolant sensor failure does not stop a running engine; it substitutes a conservative warm value and requests service. Running on a fixed enrichment value after losing coolant is how an engine is destroyed, so the fallback is warm, never cold.
- `iat_slope` - Linearised front-end gain of the intake thermistor. Register UNK-GT-009 covers the curve. IAT drives charge-temperature ignition correction and the ethanol charge-cooling term, so a wrong slope is a persistent timing error rather than a small display error.
- `oil_pressure_slope` - Oil pressure per volt; 2 bar/V is the common 0..5 V / 0..10 bar sender. A switch instead of a sender is the other possibility, and it is declared by choosing transfer = switch_hysteresis rather than by guessing a slope. Register UNK-GT-018.
- `oil_pressure_min` - Lower plausibility bound; a small negative band absorbs sender zero error.
- `oil_pressure_fault_behaviour` - Oil starvation is a latching, escalating hazard (SC-001 H-08). The declared action here only decides between substituting a value and entering the limp; the action taken is the safety group's latching cut with recovery hysteresis.
- `oil_temp_max` - Upper plausibility bound; oil above this is a failure, not a reading.
- `fuel_pressure_slope` - Rail pressure per volt at the sensor. Distinct from the pressure-compensation slope in the fuel group: this one turns volts into bars, that one corrects the delivered mass. Register UNK-GT-005.
- `fuel_pressure_fault_behaviour` - A lost pressure sensor must not mean a rich engine. The declared fallback is the conservative nominal pressure, and the low-pressure protection in the safety group is what actually restricts fuel.
- `egt_slope` - Degrees per volt at the amplifier output. A Type-K front end is about 41.9 uV per degree, so the amplifier gain sets this; 167 degC/V spans 0..835 C over 0..5 V. Probe placement matters far more than the gain and is not yet known.
- `egt_max` - Upper plausibility bound. EGT is a diagnostic input, not a control input, so this band may be generous; the protective limit is safety.egt_max_c.
- `ethanol_slope` - Ethanol fraction per volt; 0.2/V is a 0..5 V linear sensor, and most real flex sensors are 50..500 Hz, selected by transfer. Most motorcycles have no flex sensor at all, which is transfer = none with engine.fuel_ethanol_fraction set by hand instead.
- `ethanol_rate_limit` - Deliberately slow. Fuel does not change blend, so a fast transition is a sensor fault and must be rejected outright rather than blended through, which would smear a real disagreement into an undetectable wrong value.
- `ethanol_fault_behaviour` - Hold the last plausible blend for a bounded time instead of reverting at once: refuelling with a different blend genuinely happens mid-ride, and an instant step back to pure gasoline would itself be a large fuelling error.
- `battery_slope` - Divider ratio of the battery-sense input: 3.0 means a 12 V pack presents 3.0 V to the ADC. Battery voltage feeds injector dead time, coil dwell capability and both under- and over-voltage protections, so a wrong ratio is a whole-vehicle fuelling and timing error rather than a gauge error.
- `battery_max` - Upper plausibility bound; above this the reading is a wiring fault.
- `battery_fault_behaviour` - The default is the limp. With no battery measurement the ECU cannot know whether the coils are reaching primary current, so it widens no pulse and reduces authority instead of guessing.
- `ref5v_slope` - Ratio of the monitored sensor rail to the ADC reading, 1.0 for a plain divider. This channel measures the rail; it never drives it (IOM-001 AI_5V_REF).
- `ref5v_min` - Lower bound of the sensor-rail tolerance. This item is essential, and it is the least obvious channel in the product: if the 5 V rail collapses, every 5 V sensor - throttle, MAP, MAF, lambda, fuel and oil pressure, ethanol, EGT - reads incorrectly AND plausibly, so the per-channel open/short, range and rate checks are all fooled simultaneously. Monitoring the reference is the only way to detect that class of failure at all. Sensor test S9; IOM-001 sec.2.
- `ref5v_max` - Upper bound of the sensor-rail tolerance, same argument as ref5v_min.
- `ref5v_fault_behaviour` - Default 6 (invalidate_dependents) is not a tuning preference. A rail fault invalidates every ratiometric channel at the same instant, which is the NO_REFERENCE reason code in the SENS-001 sec.2 fault taxonomy.
- `lambda1_slope` - Lambda per volt on the wideband controller's analogue output. The controller linearises the sensor; the ECU never interprets raw sensor volts. Register UNK-GT-010 - neither the fitting nor the controller type is known yet.
- `lambda1_fault_behaviour` - An unready, failed or unplugged lambda sensor must never leave the closed loop running on the last value, so the declared behaviour is to mark it invalid and drop to the declared open-loop strategy (FUEL-001 test F12).
- `lambda2_slope` - Second channel. Only populate this block when hardware_lambda_channel_count actually reports a second physical fitting; an unpopulated channel must be declared with transfer = none, not left at a plausible default.
- `lambda2_transfer` - Defaults to none - a motorcycle with one exhaust fitting has one sensor.
- `lambda3_slope` - Third channel, used for a 3- or 4-cylinder build with real per-cylinder fittings.
- `lambda4_slope` - Fourth channel, for a 4-cylinder build with one fitting per cylinder.
- `sensor_filter_cutoff_hz` - Default analogue anti-alias cutoff for the slow-changing channels. The fast channels (knock, ignition current, injector current) are deliberately excluded - their bandwidth is part of the signal, not noise.
- `sensor_dual_check_enabled` - Cross-check a redundant pair (TPS1/TPS2, front/rear wheel). Enabled by default because the check costs nothing and a disagreement is otherwise invisible.
- `sensor_dual_mismatch_max` - Largest disagreement between the two members of a pair, in percent of full scale. Above this the pair is declared INCONSISTENT (SENS-001 sec.8).
- `sensor_dual_policy` - What happens on INCONSISTENT. The policy is explicit per configuration because "average the two" is only defensible for a cable throttle; for fly-by-wire, invalidating both is the honest answer.
- `sensor_frozen_window_ms` - Window over which an unchanging value is declared FROZEN. Computed from a sequence advance and a timestamp, never from "the value looks plausible" (SENS-001 sec.1, hazard H-11).
- `sensor_reference_5v_enabled` - The 5 V reference monitor. There is no safe default of off: without it, a collapsed sensor rail is undetectable and every ratiometric channel is simultaneously wrong in a plausible direction.
- `sensor_reference_5v_fault_action` - What a reference fault does to the channels that depend on it. The default invalidates them, because continuing on untrusted rails is precisely the failure this channel exists to catch. Depends on: `ref5v_min`, `ref5v_max`.

## `hardware`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0400` | `injector_driver_type` | enum | | high_z_saturated | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0401` | `ignition_driver_topology` | enum | | smart_coil | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0402` | `injector_peak_current_a` | f32 | A | 4 | 0.5 | 8 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0403` | `injector_hold_current_a` | f32 | A | 1.2 | 0.2 | 6 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0404` | `injector_max_duty_pct` | f32 | % | 85 | 5 | 100 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0405` | `ignition_peak_current_a` | f32 | A | 7.5 | 1 | 16 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0406` | `ignition_hold_current_a` | f32 | A | 3.5 | 0.5 | 10 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0407` | `ignition_max_dwell_ms` | f32 | ms | 8 | 1 | 15 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0408` | `ignition_spark_divider_ratio` | f32 | | 9 | 1 | 20 | ENGINEER | HIGH | UNVERIFIED |
| `0x0409` | `aux_output_function_0` | enum | | unassigned | 0 | 14 | ENGINEER | HIGH | UNVERIFIED |
| `0x040A` | `aux_output_function_1` | enum | | unassigned | 0 | 14 | ENGINEER | HIGH | UNVERIFIED |
| `0x040B` | `aux_output_function_2` | enum | | unassigned | 0 | 14 | ENGINEER | HIGH | UNVERIFIED |
| `0x040C` | `aux_output_function_3` | enum | | unassigned | 0 | 14 | ENGINEER | HIGH | UNVERIFIED |
| `0x040D` | `aux_output_timeout_ms` | u16 | ms | 5000 | 10 | 6e+04 | ENGINEER | HIGH | UNVERIFIED |
| `0x040E` | `aux_output_duty_max_pct` | f32 | % | 100 | 1 | 100 | ENGINEER | HIGH | UNVERIFIED |
| `0x040F` | `aux_output_flags` | bits[6] | | 0 | 0 | 63 | ENGINEER | HIGH | UNVERIFIED |
| `0x0410` | `hardware_capabilities` | bits[16] | | 0 | 0 | 65535 | READ_ONLY | CRITICAL | UNVERIFIED |
| `0x0411` | `hardware_fuel_pump_max_duty_pct` | f32 | % | 100 | 10 | 100 | ENGINEER | HIGH | UNVERIFIED |
| `0x0412` | `hardware_fuel_pump_timeout_ms` | u32 | ms | 3.6e+06 | 1000 | 8.64e+07 | ENGINEER | HIGH | UNVERIFIED |
| `0x0413` | `hardware_main_relay_feedback` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0414` | `hardware_lambda_channel_count` | u8 | | 1 | 1 | 4 | READ_ONLY | HIGH | UNVERIFIED |
| `0x0415` | `hardware_knock_channel_count` | u8 | | 0 | 0 | 2 | READ_ONLY | HIGH | UNVERIFIED |
| `0x0416` | `hardware_injector_channel_count` | u8 | | 2 | 1 | 8 | READ_ONLY | CRITICAL | UNVERIFIED |
| `0x0417` | `hardware_ignition_channel_count` | u8 | | 2 | 1 | 8 | READ_ONLY | CRITICAL | UNVERIFIED |
| `0x0418` | `hardware_driver_overtemp_c` | f32 | degC | 125 | 60 | 200 | ENGINEER | HIGH | UNVERIFIED |
| `0x0419` | `hardware_stepper_max_energised_ms` | u16 | ms | 1e+04 | 100 | 6e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x041A` | `hardware_wasted_spark_pairs` | u8 | | 0 | 0 | 2 | READ_ONLY | HIGH | UNVERIFIED |
| `0x041B` | `hardware_aux_output_count` | u8 | | 4 | 0 | 4 | READ_ONLY | MEDIUM | UNVERIFIED |

**Notes**

- `injector_driver_type` - Injector power stage. Peak-and-hold exists only where a driver provides a bounded peak phase and a bounded hold phase; with a high-Z saturated driver the ECU must command a much longer pulse for the same flow. Register UNK-GT-004.
- `ignition_driver_topology` - Coil topology, which determines whether the ECU computes dwell at all. A CDI is explicitly not assumed and is not supported without a validated profile entry (IGN-001 sec.1). Register UNK-GT-006.
- `injector_peak_current_a` - Peak current the driver sources during the opening phase. Ignored for a high-Z saturated driver, which has no peak phase.
- `injector_hold_current_a` - Hold current after opening. This is what keeps the driver's dissipation manageable over a long pulse; it is also the number that must be inside the driver IC's own limit, which is enforced in hardware.
- `injector_max_duty_pct` - Hardware duty ceiling for the injector channels. Enforced by the driver, not only by software: exceeding it is a DTC and a protective cut, never a warning (ACT-001 sec.1, hazard H-04). The safety group carries the ECU-side ceiling.
- `ignition_peak_current_a` - Primary current the gate stage sources while the coil charges. Sets how fast the coil reaches saturation and therefore how short the effective dwell has to be at high rpm.
- `ignition_hold_current_a` - Current held after saturation. Keeping the coil saturated while the spark event is required is what a smart driver does automatically; with a dumb coil the ECU must stop the charge at the right instant.
- `ignition_max_dwell_ms` - Hardware maximum dwell, enforced by a timer or by the smart driver independently of the CPU. This is a hardware property, not a software promise: with the CPU halted mid-dwell the timer still ends it (hazard H-03, test I3).
- `ignition_spark_divider_ratio` - Primary-side divider ratio used for spark-duration feedback, which turns "we commanded a spark" into "a spark event was observed". That measured event is what the safety case and the dyno analyser actually need. Register UNK-HW-006.
- `aux_output_function_0` - Function assigned to user output 0. Assignment is validated: two functions on one channel, or a function the hardware profile cannot support, is refused with a specific reason (ACT-001 sec.7).
- `aux_output_timeout_ms` - Maximum continuous ON time for one aux output, after which the driver forces it off. No output in the product has an unbounded energised duration: a solenoid welded closed is a fire, and a stepper energised forever is a thermal event (ACT-001 sec.6, test A11).
- `aux_output_duty_max_pct` - Duty ceiling for the PWM aux outputs, per output profile.
- `aux_output_flags` - Per-output behaviour flags. The default sets failsafe_off, so with the ECU unpowered or wedged every aux output is off - the only state that is unambiguously safe for an unknown device attached to it.
- `hardware_capabilities` - Capability set the firmware may assume. Derived from the hardware profile and the end-of-line test record, never hand-set: a capability is on only if the physical resource exists and passed end of line against the unit's serial number (IOM-001 sec.6). The remaining profile capabilities (CAN-FD, stepper IAC, anti-lag, internal log, spark feedback, injector diagnostics, gear and clutch sensing) live in the hardware profile binary.
- `hardware_fuel_pump_max_duty_pct` - Duty ceiling for the pump drive. A short circuit latches the output off with a bounded retry policy, so the pump is a stopped engine rather than a melted harness. Register UNK-GT-012.
- `hardware_fuel_pump_timeout_ms` - Maximum continuous pump ON time with a defined consequence, so a welded relay feedback contact cannot leave a pump running indefinitely.
- `hardware_main_relay_feedback` - True when the main relay is actually monitored. A welded main relay is a common failure; without feedback the ECU can only report that it commanded the relay, which is not the same statement.
- `hardware_lambda_channel_count` - Physical wideband input count. The controller refuses per-cylinder trim beyond this number, so it cannot imply a per-cylinder measurement that does not exist.
- `hardware_knock_channel_count` - Physical knock inputs. Zero by default because no knock sensor is known to be fitted on this engine: knock detection is impossible without one, and no claim is made without hardware (IGN-001 sec.5). Register UNK-FW-008.
- `hardware_injector_channel_count` - Physical injector output count, primary plus secondary. live_edit false.
- `hardware_ignition_channel_count` - Physical ignition output count. live_edit false.
- `hardware_driver_overtemp_c` - Driver overtemperature threshold at which the stage shuts down and raises a DTC, recovering after cooldown rather than latching (ACT-001 test A5).
- `hardware_stepper_max_energised_ms` - Hard stall/timeout limit for the idle stepper. A stepper commanded forever is a thermal event, so the maximum energised time is bounded and a return-to-zero policy follows it.
- `hardware_wasted_spark_pairs` - Number of wasted-spark coil pairs, which determines the correct pairing and polarity of the ignition outputs on a 4-cylinder build (IGN-001 test I11).
- `hardware_aux_output_count` - Number of assignable user outputs exposed by this profile. live_edit false.

## `fuel`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0700` | `injector_flow_cc_per_s` | f32 | cc/s | 10 | 0.5 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0701` | `injector_reference_pressure_kpa` | f32 | kPa | 300 | 100 | 1000 | TUNER | HIGH | UNVERIFIED |
| `0x0702` | `injector_dead_time_ms` | f32 | ms | 1 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0703` | `injector_min_pulse_ms` | f32 | ms | 1.2 | 0.1 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0704` | `injector_max_pulse_ms` | f32 | ms | 30 | 1 | 200 | TUNER | CRITICAL | UNVERIFIED |
| `0x0705` | `injector_dead_time_battery_slope` | f32 | ms/V | 0.02 | -0.5 | 0.5 | TUNER | HIGH | UNVERIFIED |
| `0x0706` | `injector_battery_comp_pct_per_v` | f32 | %/V | 1.5 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0707` | `injector_pressure_comp_pct_per_bar` | f32 | %/bar | 3 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0708` | `injector_small_pulse_comp_pct` | f32 | % | 0 | 0 | 50 | TUNER | MEDIUM | UNVERIFIED |
| `0x0709` | `staged_split_fraction` | f32 | frac | 0.5 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x070A` | `staged_activate_rpm` | u16 | rpm | 4000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x070B` | `staged_activate_load_pct` | f32 | % | 70 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x070C` | `staged_activate_primary_duty_pct` | f32 | % | 60 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x070D` | `staged_activate_boost_kpa` | f32 | kPa | 100 | 0 | 500 | TUNER | MEDIUM | UNVERIFIED |
| `0x070E` | `staged_activate_ect_c` | f32 | degC | 40 | -50 | 150 | TUNER | MEDIUM | UNVERIFIED |
| `0x070F` | `transient_tps_gain` | f32 | %/pct | 0.5 | 0 | 5 | TUNER | HIGH | UNVERIFIED |
| `0x0710` | `transient_tps_threshold` | f32 | %/s | 100 | 0 | 2000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0711` | `transient_tps_tau_ms` | u16 | ms | 200 | 10 | 5000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0712` | `transient_tps_max_pct` | f32 | % | 30 | 0 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0713` | `transient_map_gain` | f32 | %/kPa | 0.05 | 0 | 2 | TUNER | HIGH | UNVERIFIED |
| `0x0714` | `transient_map_threshold` | f32 | kPa/s | 150 | 0 | 5000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0715` | `transient_map_tau_ms` | u16 | ms | 150 | 10 | 5000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0716` | `transient_map_max_pct` | f32 | % | 25 | 0 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0717` | `transient_tip_in_gain_pct` | f32 | % | 8 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0718` | `transient_tip_out_gain_pct` | f32 | % | 6 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0719` | `wall_wet_tau_ms` | u16 | ms | 300 | 10 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x071A` | `target_lambda_bias` | f32 | lambda | 0 | -0.2 | 0.2 | TUNER | HIGH | UNVERIFIED |
| `0x071B` | `target_lambda_cold_ect_c` | f32 | C | 40 | -20 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x071C` | `target_lambda_cold_gain` | f32 | frac | 0.35 | 0 | 0.5 | TUNER | MEDIUM | UNVERIFIED |
| `0x071D` | `target_lambda_baro_gain` | f32 | frac | 1 | 0.5 | 1.5 | TUNER | MEDIUM | UNVERIFIED |
| `0x0720` | `wall_wet_max_pct` | f32 | % | 15 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0721` | `decel_cut_tps_pct` | f32 | % | 2 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0722` | `decel_cut_map_kpa` | f32 | kPa | 45 | 0 | 120 | TUNER | HIGH | UNVERIFIED |
| `0x0723` | `decel_cut_reintro_rpm` | u16 | rpm | 1800 | 0 | 2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0724` | `decel_cut_hysteresis_ms` | u16 | ms | 250 | 0 | 1e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0725` | `enrichment_accumulator_max_pct` | f32 | % | 50 | 0 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0726` | `ve_table` | table 16x16 u8 | | 0-filled | 20 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0727` | `target_lambda_table` | table 8x8 f32 | | 0-filled | 0.65 | 1.15 | TUNER | HIGH | UNVERIFIED |
| `0x0728` | `fuel_strategy` | enum | | speed_density | 0 | 3 | ENGINEER | HIGH | UNVERIFIED |
| `0x0729` | `fuel_hybrid_map_weight` | f32 | frac | 0.8 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x072A` | `learner_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x072B` | `learner_trim_max_pct` | f32 | % | 20 | 0 | 50 | TUNER | HIGH | UNVERIFIED |
| `0x072C` | `learner_rate_limit_pct_per_s` | f32 | %/s | 1 | 0.01 | 20 | TUNER | MEDIUM | UNVERIFIED |
| `0x072D` | `learner_drift_alarm_pct` | f32 | % | 10 | 1 | 50 | TUNER | MEDIUM | UNVERIFIED |
| `0x072E` | `ve_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x072F` | `ve_table_map_kpa_axis` | axis1d | kPa | 20 | 20 | 250 | TUNER | HIGH | UNVERIFIED |
| `0x0730` | `target_lambda_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0731` | `target_lambda_table_load_pct_axis` | axis1d | pct | 0 | 0 | 100 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `injector_flow_cc_per_s` - Static flow rate at the reference pressure below. This is a *measured* number in a finished product (bench flow test or datasheet); until then it is an unverified placeholder and the ECU says so. Register UNK-GT-004. Depends on: `injector_reference_pressure_kpa`.
- `injector_reference_pressure_kpa` - The pressure at which injector_flow_cc_per_s was measured. Flow and pressure are meaningless apart: quoting one without the other is a hidden calibration.
- `injector_dead_time_ms` - Dead time at the nominal battery voltage: the pulse that opens the injector but delivers no measurable fuel. Must be strictly less than the minimum pulse, and a real dead-time curve is monotonic in battery voltage (test F1).
- `injector_min_pulse_ms` - Smallest pulse the driver will accept, measured. Small-pulse compensation is applied only above this and never below it, so the injector is never asked for a pulse it cannot deliver.
- `injector_max_pulse_ms` - Software pulse ceiling, always below the duty ceiling expressed as time. This is one of the two independent brakes on over-fuelling (the other is safety.injector_duty_max_pct); hazard H-05.
- `injector_dead_time_battery_slope` - How dead time moves with battery voltage. A sagging pack opens the injector more slowly, so the pulse must widen; without this the engine goes lean every time the starter or a big injector pulse drags the rail down (test F4).
- `injector_battery_comp_pct_per_v` - Additional pulse-width compensation per volt away from the nominal rail. Deliberately small: the dominant battery effect is dead time, not driver resistance, and over-compensating is how a crank-start transient floods.
- `injector_pressure_comp_pct_per_bar` - Pulse-width correction per bar of deviation from the reference pressure. The physics is sqrt(P) for an orifice; this is the linearised, measured form of it, and a linear gain hides the fact that the true curve is a square root.
- `injector_small_pulse_comp_pct` - Shortening or widening applied in the dead-time-dominated region only. Disabled while a fuel cut is active: compensation must never resurrect a cut event (FUEL-001 sec.5).
- `staged_split_fraction` - Fraction of the demanded fuel mass delivered by the *primary* injector; the remainder goes to the secondary. Both are subject to the same min/max pulse, duty ceiling and fuel-cut gate - staged injection is a split, not an exemption.
- `staged_activate_primary_duty_pct` - Activation predicate term. Above this, single injector flow would be outside the linear range the flow number was measured over, so the split turns on.
- `staged_activate_ect_c` - Cold engines use large pulses; staged injection is normally inhibited until warm, because two small cold pulses are harder to model than one large one.
- `transient_tps_gain` - Enrichment added per percentage point of *excess* throttle rate above the threshold. Bounded, decayed and logged; a term with no bound is a prohibited defect (FUEL-001 sec.6).
- `transient_tps_tau_ms` - Decay time constant of the enrichment accumulator. Not a filter delay.
- `transient_map_gain` - Pressure-based transient gain, used where throttle rate is a poor predictor - drive-by-wire, or a MAP sensor behind a plenum with poor resolution.
- `transient_tip_in_gain_pct` - Discrete tip-in addition on a throttle opening above the threshold. Separate from the rate-based term so a tuner can take one out without losing the other.
- `transient_tip_out_gain_pct` - Tip-out pulse *reduction*, toward -not-absolutely-zero: the pulse is never driven below the minimum pulse by this term, and a fuel cut always wins.
- `wall_wet_tau_ms` - Time constant of the wall-wetting film state variable. Register UNK-FW-007 - the film model form for a motorcycle injector is literature only and has not been measured on this engine.
- `target_lambda_bias` - Additive bias on the target lambda, clamped by the lambda target bias limits. Separate from the closed-loop correction so a tuner can see the commanded target and the measured correction as two different numbers in a log.
- `target_lambda_cold_ect_c` - Engine temperature below which the target lambda is enriched for cold running. A failed ECT sensor therefore cannot command an absurdly rich mixture, because the enrichment is bounded by cold_ect_c and cold_gain together.
- `target_lambda_cold_gain` - Strength of cold enrichment as a fraction of the temperature distance below cold_ect_c. Capped at 0.5, so cold enrichment can at most halve the target lambda and never invert it.
- `target_lambda_baro_gain` - Altitude compensation gain: the factor applied per 100 kPa of barometric deficit. 1.0 disables it, which is the correct setting when the ECU has learned its baro at key-on from a MAP sensor on the manifold.
- `wall_wet_max_pct` - Bounded film mass, so a wet port cannot justify an unbounded pulse.
- `decel_cut_tps_pct` - Closed-throttle threshold for the overrun fuel cut. The cut decision is made once, in a dedicated arbiter, and applied as a hard gate at the output stage after every other calculation (FUEL-001 sec.7).
- `decel_cut_map_kpa` - Manifold threshold for the overrun cut. TPS alone is a poor overrun detector under drive-by-wire or with a sloppy linkage, so pressure is the second term.
- `decel_cut_reintro_rpm` - Reintroduction requires rpm above this AND the hysteresis timer elapsed AND throttle or MAP above the threshold AND the clutch/gear condition satisfied - all four, not any one.
- `decel_cut_hysteresis_ms` - Without hysteresis a chattering throttle re-opens and closes the injector every few cycles, which is how a saturated driver dies.
- `enrichment_accumulator_max_pct` - Ceiling on the single enrichment accumulator. There is exactly one accumulator; independent enrichers must not be able to stack silently (FUEL-001 sec.6).
- `ve_table` - Volumetric efficiency, 0..200 %, against rpm and manifold pressure. The axis values come from companion dimension items so a 2-cylinder build drives the same allocation over a narrower range. Interpolated bilinearly with axis monotonicity validated; a non-monotonic axis is refused at write time.
- `target_lambda_table` - Target lambda against rpm and load. The commanded value is this table times the ethanol blend correction, times the mode target, plus the temperature correction and the bounded closed-loop bias - in that order, and finally clamped by the lean safety envelope (hazard H-05 / test F17).
- `fuel_strategy` - Load estimator. Every strategy reports (value, source, confidence) and a strategy whose sensor is invalid fails to a declared fallback recorded in the correction vector - it never silently produces a number (FUEL-001 sec.2).
- `fuel_hybrid_map_weight` - Weight on the MAP term in hybrid. A named, calibrated, logged term - the point of listing it separately is that it is not hidden inside an average.
- `learner_enabled` - 4D long-term learner. Off by default: convergence and drift in traffic are unproven for this engine, and a self-inflicted lean is worse than a static error. Register UNK-FW-011.
- `learner_trim_max_pct` - Absolute bound on any learned trim. On reaching it the cell freezes and raises a DTC; it never continues to walk (test F16).
- `learner_drift_alarm_pct` - If the *average* learned trim across the table exceeds this, the ECU raises a drift alarm: that pattern indicates a sensor or fuel problem, not a calibration that wants updating.
- `ve_table_rpm_axis` - rpm breakpoints for ve_table: 16 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `ve_table_map_kpa_axis` - map_kpa breakpoints for ve_table: 16 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `target_lambda_table_rpm_axis` - rpm breakpoints for target_lambda_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `target_lambda_table_load_pct_axis` - load_pct breakpoints for target_lambda_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `ignition`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0800` | `ignition_base_table` | table 16x16 f32 | | 0-filled | -10 | 35 | TUNER | CRITICAL | UNVERIFIED |
| `0x0801` | `dwell_table` | table 8x8 f32 | | 0-filled | 1 | 8 | TUNER | CRITICAL | UNVERIFIED |
| `0x0802` | `dwell_min_ms` | f32 | ms | 1 | 0.1 | 5 | TUNER | HIGH | UNVERIFIED |
| `0x0803` | `dwell_max_ms` | f32 | ms | 8 | 1 | 20 | TUNER | CRITICAL | UNVERIFIED |
| `0x0804` | `coil_on_timeout_ms` | f32 | ms | 20 | 1 | 100 | TUNER | CRITICAL | UNVERIFIED |
| `0x0805` | `ignition_cyl0_trim_deg` | f32 | deg | 0 | -10 | 10 | TUNER | HIGH | UNVERIFIED |
| `0x0806` | `ignition_cyl1_trim_deg` | f32 | deg | 0 | -10 | 10 | TUNER | HIGH | UNVERIFIED |
| `0x0807` | `ignition_cyl2_trim_deg` | f32 | deg | 0 | -10 | 10 | TUNER | HIGH | UNVERIFIED |
| `0x0808` | `ignition_cyl3_trim_deg` | f32 | deg | 0 | -10 | 10 | TUNER | HIGH | UNVERIFIED |
| `0x0809` | `ignition_high_rpm_retard` | f32 | deg/1000rpm | 1 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x080A` | `ignition_iat_correction_deg` | f32 | deg | -2 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x080B` | `ignition_ect_correction_deg` | f32 | deg | -3 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x080C` | `ignition_battery_retard_below_v` | f32 | V | 11 | 6 | 14 | TUNER | HIGH | UNVERIFIED |
| `0x080D` | `ignition_battery_retard_deg` | f32 | deg | 6 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x080E` | `ignition_user_trim_max_deg` | f32 | deg | 5 | 0 | 15 | TUNER | HIGH | UNVERIFIED |
| `0x080F` | `ignition_strategy` | enum | | static_table | 0 | 2 | ENGINEER | HIGH | UNVERIFIED |
| `0x0810` | `spark_duration_min_us` | u16 | us | 200 | 0 | 5000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0811` | `spark_duration_max_us` | u32 | us | 1e+05 | 1000 | 5e+06 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0812` | `ignition_base_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0813` | `ignition_base_table_load_pct_axis` | axis1d | pct | 0 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0814` | `dwell_table_volt_axis` | axis1d | V | 8 | 8 | 18 | TUNER | HIGH | UNVERIFIED |
| `0x0815` | `dwell_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `ignition_base_table` - Base advance in crank degrees BTDC. Every correction is added to this and the sum is clamped by the hard safety envelope *last*, so no combination of corrections and user trim can push past it (hazard H-02, test I12).
- `dwell_table` - Commanded dwell in ms against battery voltage and rpm. The physics underneath is primary - primary inductance, resistance, target current - but the shipped default is unverified until it is measured with a current probe on a real coil. Register UNK-GT-006.
- `dwell_min_ms` - Software floor. Below this a coil does not reach primary current and there is no spark at all, so commanding less wastes the event.
- `dwell_max_ms` - Software ceiling. Must not exceed the hardware-enforced value in hardware.ignition_max_dwell_ms, and the two are independent by design: a hung CPU must still be unable to cook a coil.
- `coil_on_timeout_ms` - Independent watchdog on total coil-on time, separate from the dwell control logic and from the main loop. Sync loss during dwell de-energises the coil immediately; no stored energy is left in it (test I6).
- `ignition_cyl0_trim_deg` - Per-cylinder timing trim, applied before the safety clamp. A parallel twin with a shared exhaust can genuinely need a split; a four with equal lengths must be left at zero rather than trimmed by feel.
- `ignition_high_rpm_retard` - Retard applied per 1000 rpm above the point the available spark energy stops keeping up. Rather than being hidden in the map, it is a named term in the correction vector.
- `ignition_iat_correction_deg` - Timing correction at the hot end of the intake-air range; normally negative, because warm dense charge burns faster. The magnitude is engine-specific.
- `ignition_ect_correction_deg` - Timing correction at the hot end of the coolant range; usually negative, and normally larger in magnitude than the IAT term because it reflects real knock tendency rather than charge density.
- `ignition_battery_retard_below_v` - Battery voltage below which the retard policy engages.
- `ignition_battery_retard_deg` - Retard applied at that voltage. Low voltage means the coil is not reaching primary current, so the honest response is less timing, not more (test I5).
- `ignition_user_trim_max_deg` - Bound on the per-cell user trim, so a trim cannot be used to smuggle advance past the safety envelope by another route.
- `ignition_strategy` - Which correction sources are allowed to modify the base map. static_table is what a dyno session wants: it is reproducible, because nothing is adapting under the measurement.
- `spark_duration_min_us` - Shortest spark-duration feedback pulse treated as a real spark event. This is what turns "we commanded a spark" into "a spark was observed"; a mismatch raises a DTC instead of being invisible.
- `spark_duration_max_us` - Upper bound of a plausible spark event; beyond it the feedback is noise.
- `ignition_base_table_rpm_axis` - rpm breakpoints for ignition_base_table: 16 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `ignition_base_table_load_pct_axis` - load_pct breakpoints for ignition_base_table: 16 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `dwell_table_volt_axis` - volt breakpoints for dwell_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `dwell_table_rpm_axis` - rpm breakpoints for dwell_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `lambda`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0900` | `narrowband_switch_threshold_v` | f32 | V | 0.45 | 0.05 | 0.95 | ENGINEER | HIGH | UNVERIFIED |
| `0x0901` | `narrowband_dwell_time_ms` | u16 | ms | 100 | 1 | 5000 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0902` | `wideband_controller_type` | enum | | none | 0 | 4 | ENGINEER | HIGH | UNVERIFIED |
| `0x0903` | `wideband_heater_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0904` | `wideband_heater_duty_pct` | f32 | % | 50 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0905` | `wideband_warmup_timeout_ms` | u32 | ms | 3e+04 | 100 | 3e+06 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0906` | `lambda_target_bias_min` | f32 | lambda | -0.05 | -0.5 | 0 | TUNER | HIGH | UNVERIFIED |
| `0x0907` | `lambda_target_bias_max` | f32 | lambda | 0.1 | 0 | 0.5 | TUNER | HIGH | UNVERIFIED |
| `0x0908` | `lambda_closed_loop_gain` | f32 | | 0.5 | 0 | 5 | TUNER | HIGH | UNVERIFIED |
| `0x0909` | `lambda_closed_loop_timeout_ms` | u16 | ms | 200 | 10 | 1e+04 | TUNER | HIGH | UNVERIFIED |
| `0x090A` | `lambda_cyl0_trim_pct` | f32 | % | 0 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x090B` | `lambda_cyl1_trim_pct` | f32 | % | 0 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x090C` | `lambda_cyl2_trim_pct` | f32 | % | 0 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x090D` | `lambda_cyl3_trim_pct` | f32 | % | 0 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x090E` | `lambda_cyl0_trim_min` | f32 | % | -20 | -50 | 0 | TUNER | HIGH | UNVERIFIED |
| `0x090F` | `lambda_cyl0_trim_max` | f32 | % | 20 | 0 | 50 | TUNER | HIGH | UNVERIFIED |
| `0x0910` | `lambda_cyl1_trim_min` | f32 | % | -20 | -50 | 0 | TUNER | HIGH | UNVERIFIED |
| `0x0911` | `lambda_cyl1_trim_max` | f32 | % | 20 | 0 | 50 | TUNER | HIGH | UNVERIFIED |
| `0x0912` | `lambda_cyl2_trim_min` | f32 | % | -20 | -50 | 0 | TUNER | HIGH | UNVERIFIED |
| `0x0913` | `lambda_cyl2_trim_max` | f32 | % | 20 | 0 | 50 | TUNER | HIGH | UNVERIFIED |
| `0x0914` | `lambda_cyl3_trim_min` | f32 | % | -20 | -50 | 0 | TUNER | HIGH | UNVERIFIED |
| `0x0915` | `lambda_cyl3_trim_max` | f32 | % | 20 | 0 | 50 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `narrowband_switch_threshold_v` - Switching voltage of a narrowband sensor. The ECU uses it for a switching period and a duty-based average only; a narrowband sensor is *never* treated as a wide-range AFR readout. Register UNK-GT-010.
- `narrowband_dwell_time_ms` - Minimum time at each state before the sensor is considered to have switched. Without it a noisy edge produces a nonsense average lambda.
- `wideband_controller_type` - How lambda is obtained. The native path requires the CJ125 to be physically present (CAP_WIDEBAND); without it no wideband feature is offered at all rather than being simulated (IOM-001 sec.6).
- `wideband_heater_enabled` - Wideband heater. Off by default: it is a large continuous load on a motorcycle charging system, and it must be actively asked for.
- `wideband_heater_duty_pct` - Heater duty. High enough to reach the sensor's operating temperature, low enough not to cook it; a hot LSU destroys itself, so this is a bounded value and not a "full on".
- `wideband_warmup_timeout_ms` - Time allowed for the sensor to be considered ready. Before it expires, closed loop is not entered at all - an unheated LSU reports confidently and wrongly.
- `lambda_target_bias_min` - Most negative bias the long-term learner may apply to the target. A learner that can walk the mixture arbitrarily is a self-inflicted lean, so the bias is bounded per cell, per cylinder and globally.
- `lambda_target_bias_max` - Most positive bias, richer than the table value. A0 asymmetric on purpose: drifting rich is recoverable in traffic, drifting lean is not.
- `lambda_closed_loop_gain` - Proportional correction per unit lambda error. The loop bandwidth is deliberately limited: the time constant must be much longer than the sensor response and much shorter than the transient time constant. A controller is never shipped with an unmeasured gain (UNK-FW-010).
- `lambda_closed_loop_timeout_ms` - A controller that reports "sensor not ready" is invalid and the loop is not entered; a controller that stops reporting entirely times out here into a declared fallback rather than a held value.
- `lambda_cyl0_trim_pct` - Per-cylinder fuelling trim. Only meaningful where a per-cylinder channel physically exists; the validator refuses this feature beyond hardware_lambda_channel_count, so a single common-exhaust sensor can never be made to look like four.
- `lambda_cyl0_trim_min` - Hard lower bound on cylinder 0 trim, applied after every correction.

## `knock`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0A00` | `knock_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0A01` | `knock_cyl0_window_center_deg` | f32 | deg | 10 | -20 | 90 | TUNER | HIGH | UNVERIFIED |
| `0x0A02` | `knock_cyl1_window_center_deg` | f32 | deg | 10 | -20 | 90 | TUNER | HIGH | UNVERIFIED |
| `0x0A03` | `knock_cyl2_window_center_deg` | f32 | deg | 10 | -20 | 90 | TUNER | HIGH | UNVERIFIED |
| `0x0A04` | `knock_cyl3_window_center_deg` | f32 | deg | 10 | -20 | 90 | TUNER | HIGH | UNVERIFIED |
| `0x0A05` | `knock_cyl0_window_size_deg` | f32 | deg | 30 | 5 | 180 | TUNER | HIGH | UNVERIFIED |
| `0x0A06` | `knock_cyl1_window_size_deg` | f32 | deg | 30 | 5 | 180 | TUNER | HIGH | UNVERIFIED |
| `0x0A07` | `knock_cyl2_window_size_deg` | f32 | deg | 30 | 5 | 180 | TUNER | HIGH | UNVERIFIED |
| `0x0A08` | `knock_cyl3_window_size_deg` | f32 | deg | 30 | 5 | 180 | TUNER | HIGH | UNVERIFIED |
| `0x0A09` | `knock_filter_low_hz` | f32 | Hz | 5000 | 100 | 2e+04 | TUNER | MEDIUM | SOURCE-DERIVED IGN-001 sec.5 (band-pass 5-20 kHz) |
| `0x0A0A` | `knock_filter_high_hz` | f32 | Hz | 2e+04 | 1000 | 4e+04 | TUNER | MEDIUM | SOURCE-DERIVED IGN-001 sec.5 (band-pass 5-20 kHz) |
| `0x0A0B` | `knock_threshold` | f32 | | 0.5 | 0.01 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0A0C` | `knock_noise_floor` | f32 | | 0.1 | 0.001 | 10 | TUNER | HIGH | UNVERIFIED |
| `0x0A0D` | `knock_retard_per_event_deg` | f32 | deg | 2 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0A0E` | `knock_max_total_retard_deg` | f32 | deg | 12 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0A0F` | `knock_recovery_deg_per_s` | f32 | deg/s | 2 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0A10` | `knock_escalation_2_retard_deg` | f32 | deg | 6 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0A11` | `knock_escalation_3_events` | u16 | | 10 | 1 | 1000 | TUNER | HIGH | UNVERIFIED |
| `0x0A12` | `knock_event_window_ms` | u16 | ms | 500 | 10 | 6e+04 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `knock_enabled` - Off by default because no knock sensor is known to be fitted on this engine. Knock detection is impossible without one, and the ECU must not pretend otherwise. Register UNK-FW-008.
- `knock_cyl0_window_center_deg` - Crank angle after this cylinder's TDC at which its knock window is centred. The window is placed from the firing angle, not guessed, so a change of firing order moves it with it.
- `knock_cyl0_window_size_deg` - Half-width is *not* implied: a wide window catches more knock and more noise, a narrow one misses a slow burn. The size is a calibration because the block's resonant frequency is (UNK-FW-008).
- `knock_filter_low_hz` - Lower corner of the conditioning band-pass.
- `knock_filter_high_hz` - Upper corner. The band is narrow on purpose: widening it admits combustion noise and mechanical ring, which then looks like knock everywhere.
- `knock_threshold` - Envelope level counted as a knock event. It is referenced to the measured noise floor below, not to an absolute voltage, so a different mounting position does not invalidate it.
- `knock_noise_floor` - Estimated noise floor of the conditioned signal. Without it a disconnected sensor masquerades as "no knock" - or as constant knock, depending on the noise. Both failure modes are damaging.
- `knock_retard_per_event_deg` - Retard requested per detected event, accumulated per cylinder.
- `knock_max_total_retard_deg` - Ceiling on accumulated retard. Beyond it the response escalates to a cut rather than continuing to retard into the engine.
- `knock_recovery_deg_per_s` - Rate at which retard is released. Recovery is deliberately slow: snapping back to full advance the moment the signal quietens is how engines get hurt, because the knock that mattered is still ringing.
- `knock_escalation_2_retard_deg` - Total retard at which persistent knock escalates to an ignition cut, a second stage beyond simple retard.
- `knock_escalation_3_events` - Events within the window below which escalation escalates again, to shutdown.
- `knock_event_window_ms` - Window over which knock events are counted for the escalation decision. A count without a window is meaningless - it would escalate on any total.

## `idle`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0B00` | `idle_strategy` | enum | | off | 0 | 4 | ENGINEER | HIGH | UNVERIFIED |
| `0x0B01` | `idle_pwm_frequency_hz` | u16 | Hz | 1000 | 20 | 5e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B02` | `idle_stepper_steps` | u16 | steps | 100 | 10 | 1000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B03` | `idle_stepper_microsteps` | u8 | | 16 | 1 | 32 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B04` | `idle_stepper_max_energised_ms` | u16 | ms | 1e+04 | 100 | 6e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B05` | `idle_valve_duty_min_pct` | f32 | % | 0 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B06` | `idle_valve_duty_max_pct` | f32 | % | 60 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B07` | `idle_dashpot_gain` | f32 | | 0.05 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B08` | `idle_antistall_rpm_drop` | u16 | rpm | 300 | 0 | 2000 | TUNER | HIGH | UNVERIFIED |
| `0x0B09` | `idle_antistall_torque_scale` | f32 | | 0.3 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0B0A` | `idle_antistall_hold_ms` | u16 | ms | 5000 | 0 | 6e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0B0B` | `idle_entry_rpm` | u16 | rpm | 1400 | 400 | 4000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B0C` | `idle_exit_rpm` | u16 | rpm | 1800 | 400 | 5000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B0D` | `idle_open_loop_gain` | f32 | | 0.2 | 0 | 5 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B0E` | `idle_closed_loop_gain` | f32 | | 0.1 | 0 | 5 | TUNER | MEDIUM | UNVERIFIED |
| `0x0B0F` | `idle_enable_at_ect_c` | f32 | degC | 40 | -50 | 150 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `idle_strategy` - How idle is controlled. Most parallel twins have no idle actuator at all and idle is ignition plus a bypass, so none is the honest default. Register UNK-GT-011.
- `idle_pwm_frequency_hz` - PWM carrier for the idle valve. Above the audible band a valve that chatters is a symptom of too low a frequency or too little current.
- `idle_stepper_steps` - Full-travel step count. A stepper commanded past its end stops silently and the calibration has no idea, so travel is a stated number.
- `idle_stepper_max_energised_ms` - Maximum energised time before the stepper is de-energised and returned toward zero. A stepper that is commanded forever is a thermal event, not a convenience (ACT-001 sec.4, test A12).
- `idle_valve_duty_max_pct` - Bound on idle air. A stuck-open idle valve floods the engine and floods the exhaust, so the ceiling is a calibration value with a reason, not a guess.
- `idle_dashpot_gain` - Proportional gain of the air dashpot on rpm error. Terminology carried over from a carburettor because the physics is the same; it is a PID term, not a legacy artefact.
- `idle_antistall_rpm_drop` - How far below the target the idle controller may let rpm fall before the anti-stall assist engages. This is the last defence against a stall in traffic, and it engages on rpm, never on a timer.
- `idle_antistall_torque_scale` - Fraction of full authority available to the anti-stall assist. Bounded below one on purpose: a stall assist that can command full torque on a stationary engine is a rev limiter wearing a different name.
- `idle_antistall_hold_ms` - Maximum time the assist may stay engaged. Every latching request has a maximum lifetime; a latch with no release path is a prohibited defect.
- `idle_exit_rpm` - Hysteresis on idle entry/exit, so a rider feathering the throttle does not make the idle controller and the rider fight each other.
- `idle_open_loop_gain` - Feed-forward term - a lookup of position against coolant temperature.
- `idle_closed_loop_gain` - Integral authority on rpm error, per second. Kept low deliberately: an integral term large enough to react quickly is also large enough to hunt.
- `idle_enable_at_ect_c` - Coolant temperature above which idle control takes over. Below it, cold start enrichment owns the mixture and an idle controller would fight it. Depends on: `idle_rpm_target`.

## `boost`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0C00` | `boost_target_table` | table 8x8 f32 | | 0-filled | 0 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x0C01` | `boost_closed_loop_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0C02` | `boost_p_gain` | f32 | | 0.01 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0C03` | `boost_i_gain` | f32 | | 0.002 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0C04` | `boost_d_gain` | f32 | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C05` | `boost_integral_limit_pct` | f32 | % | 30 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0C06` | `boost_wastegate_min_duty_pct` | f32 | % | 0 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C07` | `boost_wastegate_max_duty_pct` | f32 | % | 90 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C08` | `boost_open_loop_gain` | f32 | | 1 | 0 | 10 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C09` | `boost_overboost_limit_kpa` | f32 | kPa | 220 | 0 | 1000 | TUNER | CRITICAL | UNVERIFIED |
| `0x0C0A` | `boost_closed_loop_rpm_min` | u16 | rpm | 2000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C0B` | `boost_closed_loop_rpm_max` | u16 | rpm | 8000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C0C` | `boost_solenoid_pwm_hz` | u16 | Hz | 1000 | 20 | 5e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0C0D` | `boost_target_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0C0E` | `boost_target_table_load_pct_axis` | axis1d | pct | 0 | 0 | 100 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `boost_target_table` - Target manifold pressure in kPa absolute against rpm and load. Boost is a requesting subsystem, not an override: overboost protection outranks it in the torque manager (TQM-001 rank 10).
- `boost_closed_loop_enabled` - Off by default. A naturally aspirated engine must not have a boost controller quietly commanding a solenoid, and closed loop with unmeasured gains is exactly the oscillation risk UNK-FW-010 is about.
- `boost_p_gain` - Proportional gain, duty fraction per kPa of error. The stability margin must be measured in SIL/HIL before this loop is ever enabled.
- `boost_d_gain` - Derivative term, left at zero: the measured signal is already noisy and the derivative amplifies exactly the part that is not information.
- `boost_integral_limit_pct` - Anti-windup clamp on the integral term. A wound-up boost controller is a boost controller that overshoots the moment the limit releases.
- `boost_wastegate_min_duty_pct` - Duty the controller may reduce to. A floor above zero models a wastegate that cannot fully close - a real and frequently overlooked mechanical fact.
- `boost_wastegate_max_duty_pct` - Duty the controller may command. Left below 100 % so a failed solenoid does not become a stuck-open gate.
- `boost_open_loop_gain` - Feed-forward from the target table before any feedback, so a step demand does not rely on the loop to find the level from scratch.
- `boost_overboost_limit_kpa` - Boost-controller protection threshold: the emergency trip the boost subsystem takes, which must sit ABOVE the design maximum so a healthy closed loop never reaches it. safety.boost_max_kpa is the non-bypassable envelope below it. Depends on: `boost_max_kpa`.
- `boost_closed_loop_rpm_min` - Below this rpm the loop is open: at low speed a small pressure error is a large mass error and the feedback is not informative.
- `boost_closed_loop_rpm_max` - Upper end of the closed-loop band, where the valve starts to lag.
- `boost_solenoid_pwm_hz` - Solenoid PWM carrier. Bounded duty, and the output is off on overboost, sync loss or a closed-loop fault (ACT-001 sec.6).
- `boost_target_table_rpm_axis` - rpm breakpoints for boost_target_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `boost_target_table_load_pct_axis` - load_pct breakpoints for boost_target_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `vvt`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0D00` | `vvt_enabled` | bool | | 0 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x0D01` | `vvt_target_table` | table 8x8 f32 | | 0-filled | -35 | 35 | TUNER | HIGH | UNVERIFIED |
| `0x0D02` | `vvt_min_duty_pct` | f32 | % | 20 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0D03` | `vvt_max_duty_pct` | f32 | % | 80 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0D04` | `vvt_p_gain` | f32 | | 0.5 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0D05` | `vvt_i_gain` | f32 | | 0.1 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x0D06` | `vvt_d_gain` | f32 | | 0 | 0 | 20 | TUNER | MEDIUM | UNVERIFIED |
| `0x0D07` | `vvt_integral_limit_pct` | f32 | % | 20 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x0D08` | `vvt_position_tolerance_deg` | f32 | deg | 2 | 0.1 | 15 | TUNER | HIGH | UNVERIFIED |
| `0x0D09` | `vvt_feedback_enabled` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0D0A` | `vvt_pwm_hz` | u16 | Hz | 500 | 20 | 5e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0D0B` | `vvt_target_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0D0C` | `vvt_target_table_load_pct_axis` | axis1d | pct | 0 | 0 | 100 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `vvt_enabled` - Off by default. The GT650 is an overhead-cam engine with no phaser, and enabling VVT on hardware that does not have it would be exactly the fabricated capability the product is required not to claim (IOM-001 sec.6).
- `vvt_target_table` - Target phaser angle in crank degrees against rpm and load. Positive is advanced; the actual authority is limited by the actuator and verified by position feedback, never by this table alone.
- `vvt_min_duty_pct` - Duty the controller may reduce to. A floor above zero keeps oil pressure to the phaser above the unlock threshold at high rpm.
- `vvt_max_duty_pct` - Duty ceiling, held below 100 % so a failed solenoid is not a fully open phaser.
- `vvt_p_gain` - Proportional gain, duty fraction per degree of position error.
- `vvt_integral_limit_pct` - Anti-windup clamp, for the same reason as boost - a wound-up controller is a stored overshoot waiting for the moment the limit releases.
- `vvt_position_tolerance_deg` - Position error beyond which closed loop stops integrating and raises a DTC. The point is to catch a phaser that is not following its command before the timing is silently wrong.
- `vvt_feedback_enabled` - Whether a position sensor exists. Off means the controller is open loop and the ECU must say so in the capability frame rather than implying verification.
- `vvt_target_table_rpm_axis` - rpm breakpoints for vvt_target_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `vvt_target_table_load_pct_axis` - load_pct breakpoints for vvt_target_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `flexfuel`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1600` | `flexfuel_enabled` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1601` | `flexfuel_sensor_type` | enum | | none | 0 | 3 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1602` | `flexfuel_cold_start_enrich_pct` | f32 | % | 20 | 0 | 200 | TUNER | HIGH | UNVERIFIED |
| `0x1603` | `flexfuel_cold_start_below_ect_c` | f32 | degC | 20 | -50 | 150 | TUNER | MEDIUM | UNVERIFIED |
| `0x1604` | `flexfuel_ignition_blend_deg` | f32 | deg | 3 | -15 | 15 | TUNER | HIGH | UNVERIFIED |
| `0x1605` | `flexfuel_ignition_blend_max_deg` | f32 | deg | 8 | -20 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x1606` | `flexfuel_min_ethanol_fraction` | f32 | frac | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1607` | `flexfuel_max_ethanol_fraction` | f32 | frac | 0.85 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1608` | `flexfuel_switch_hysteresis_frac` | f32 | frac | 0.02 | 0 | 0.2 | TUNER | MEDIUM | UNVERIFIED |
| `0x1609` | `flexfuel_invalid_action` | enum | | revert_to_gasoline | 0 | 3 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `flexfuel_enabled` - Off by default: a motorcycle with no flex sensor has a fixed blend, set once in the engine group. Enabling this without a sensor would make the ECU blend from noise.
- `flexfuel_sensor_type` - How the ethanol fraction is obtained. A CAN source must have an explicit timeout and a defined fallback, or a dead adapter would freeze the blend.
- `flexfuel_cold_start_enrich_pct` - Cold-start enrichment at full ethanol. Ethanol needs more mixture to reach the same lambda target, and on top of that its higher charge temperature changes how that mixture burns cold.
- `flexfuel_ignition_blend_deg` - Timing change at full ethanol. Ethanol's higher charge temperature and latent heat mean *more* advance is usually needed, not less - the opposite of the instinct imported from gasoline, and the reason this is a calibration with a sign rather than a fixed rule.
- `flexfuel_ignition_blend_max_deg` - Ceiling on the ignition blend term, so a misreading blend cannot move timing without limit.
- `flexfuel_min_ethanol_fraction` - Lowest fraction accepted from the sensor. Below this the reading is treated as a fault rather than blended, because a 0 % floor is where a disconnected sensor would otherwise land.
- `flexfuel_max_ethanol_fraction` - Highest fraction accepted. Deliberately below 1.0 by default: a genuine E100 fill is rare enough on a motorcycle that being told about it is more useful than silently running a blend the calibration was never set up for.
- `flexfuel_switch_hysteresis_frac` - Hysteresis on switching the *active* blend between two adjacent calibrated points, so a signal hovering at a boundary does not dither the whole model.
- `flexfuel_invalid_action` - What happens when the blend sensor fails. Reverting to the fixed blend in the engine group is the default, because a *wrong* blend is a persistent fuelling error while a *stale* one is at least visible.

## `gear`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1700` | `gear_ratio_1` | f32 | :1 | 2.2 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1701` | `gear_ratio_2` | f32 | :1 | 1.6 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1702` | `gear_ratio_3` | f32 | :1 | 1.25 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1703` | `gear_ratio_4` | f32 | :1 | 1 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1704` | `gear_ratio_5` | f32 | :1 | 0.85 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1705` | `gear_ratio_6` | f32 | :1 | 0.72 | 0.5 | 6 | TUNER | MEDIUM | UNVERIFIED |
| `0x1706` | `final_drive_ratio` | f32 | :1 | 3.5 | 0.5 | 10 | TUNER | MEDIUM | UNVERIFIED |
| `0x1707` | `primary_ratio` | f32 | :1 | 1 | 0.5 | 4 | TUNER | MEDIUM | UNVERIFIED |
| `0x1708` | `gear_ratio_count` | u8 | | 6 | 1 | 6 | READ_ONLY | HIGH | UNVERIFIED |
| `0x1709` | `tyre_circumference_m` | f32 | m | 1.9 | 0.5 | 3.5 | TUNER | MEDIUM | UNVERIFIED |
| `0x170A` | `reverse_detection_enabled` | bool | | 0 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x170B` | `reverse_speed_threshold_kph` | f32 | km/h | 5 | 0 | 30 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x170C` | `gear_strategy_table` | table 8x8 f32 | | 0-filled | 0 | 150 | TUNER | MEDIUM | UNVERIFIED |
| `0x170D` | `gear_strategy_table_gear_axis` | axis1d | gear | 1 | 1 | 8 | TUNER | HIGH | UNVERIFIED |
| `0x170E` | `gear_strategy_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `gear_ratio_1` - Transmission ratios. These are facts about the gearbox, not tuning choices, so they are only as good as the service manual - and the GT650 gearbox ratio is not published. A wrong ratio does not hurt the engine; it makes gear-derived speed, shift scheduling, launch and the pit limiter all quietly wrong.
- `primary_ratio` - Primary reduction, between crank and gearbox input. live_edit false.
- `gear_ratio_count` - Number of forward ratios this configuration declares. The remaining gear_ratio_* items stay in the image but are not used, which keeps ids stable if a different gearbox is configured. live_edit false.
- `tyre_circumference_m` - Rolling circumference of the driven tyre. Together with the ratios this is what turns engine rpm into road speed, and it is the second half of the kinematic consistency check against the wheel-speed sensor (SENS-001 sec.8).
- `reverse_detection_enabled` - Most motorcycles have no reverse. It stays off until a gear sensor or a kinematic derivation exists, and neither is known for this bike. Register UNK-GT-015.
- `reverse_speed_threshold_kph` - Below this road speed, rpm and wheel speed disagreeing in the reverse direction is treated as reverse. Small because a false positive at speed means an unintended launch-direction command.
- `gear_strategy_table` - Per-gear torque scale, percent, against gear and rpm. This is a *request*, not a command: it enters the torque manager as a requester and loses to anything protective, exactly like every other feature.
- `gear_strategy_table_gear_axis` - gear breakpoints for gear_strategy_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `gear_strategy_table_rpm_axis` - rpm breakpoints for gear_strategy_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `torque`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0E00` | `torque_scale_min` | f32 | | 0 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x0E01` | `torque_scale_max` | f32 | | 1 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0E02` | `torque_reduction_step` | f32 | | 0.01 | 0.001 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0E03` | `retard_per_unit_torque_loss` | f32 | deg | 5 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0E04` | `retard_equivalent_max_deg` | f32 | deg | 12 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0E05` | `requester_enable_bits` | bits[16] | | 0 | 0 | 65535 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0E06` | `torque_latch_max_ms` | u32 | ms | 3e+04 | 100 | 6e+06 | ENGINEER | HIGH | UNVERIFIED |
| `0x0E07` | `torque_trace_enabled` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |

**Notes**

- `torque_scale_min` - Lowest authority a requester may reduce to. 0.0 is a cut; a product that cannot express a cut through the same path it uses for a 5 % reduction is a product with two paths to get the arbitration wrong.
- `torque_scale_max` - Ceiling of the scale. It is 1.0 and it cannot exceed 1.0: arbitration scales *requests*, and there is no requester that may raise one above the base request (TQM-001 R4).
- `torque_reduction_step` - Resolution of the emitted scale. Coarse steps make a continuous requester (traction) quantised, which shows up as a hunting driveline; too fine a step costs nothing but hides the fact that a "continuous" request is a sampled one.
- `retard_per_unit_torque_loss` - Timing removed per unit of (1 - allowed_scale) when fuel has been cut. This is the retard_equivalent of TQM-001 sec.3, and it is explicitly *not* a guess: the relationship between advance and torque is engine-specific, so this number has to come from a measured torque curve, not from a rule of thumb.
- `retard_equivalent_max_deg` - Ceiling on the retard_equivalent term. A fully cut fuel request must not also remove unbounded spark, or a rev limiter would command a total loss of combustion rather than a controlled one.
- `requester_enable_bits` - Which requesters are wired into the arbiter, in the fixed rank order of TQM-001 R2. This is an *enable* mask, not a reordering: clearing a bit removes a requester, and no arrangement of these bits can change which requester wins. Bit 0 is the strongest reduction.
- `torque_latch_max_ms` - Maximum lifetime of any latching request (TQM-001 R7). A latch with no release path is a prohibited defect - it is a slow brick in software form - so the bound is structural and applies to every requester at once.
- `torque_trace_enabled` - Emit the arbitration trace: winning requester, applied scale, ignition request, fuel request. Traceability is the difference between an arbitration table that is meaningful and one that is aspirational (R6, test TM9). On by default because it is cheap and it is the only way to explain a single cylinder's behaviour after the fact.

## `launch`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0F00` | `launch_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0F01` | `launch_target_rpm` | u16 | rpm | 5000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0F02` | `launch_hold_rpm_band` | u16 | rpm | 400 | 50 | 3000 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F03` | `launch_retard_deg` | f32 | deg | 8 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x0F04` | `launch_torque_scale` | f32 | | 0.6 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0F05` | `launch_ramp_time_ms` | u16 | ms | 2000 | 0 | 6e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F06` | `launch_timeout_ms` | u16 | ms | 5000 | 100 | 6e+04 | TUNER | HIGH | UNVERIFIED |
| `0x0F07` | `launch_abort_torque_scale` | f32 | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x0F08` | `launch_wheelie_mitigation_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F09` | `launch_wheelie_front_lift_gain` | f32 | | 0.5 | 0 | 5 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F0A` | `launch_fuel_trim_pct` | f32 | % | 5 | -50 | 50 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F0B` | `launch_min_speed_kph` | f32 | km/h | 0 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x0F0C` | `launch_max_boost_kpa` | f32 | kPa | 100 | 0 | 500 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `launch_enabled` - Off by default: launch control also needs a clutch or shift switch, and neither is known to be fitted. Register UNK-GT-016.
- `launch_target_rpm` - Engine speed held during a launch. It is a *request* through the torque manager, so a rev limiter or a knock event at that speed wins without any special case.
- `launch_retard_deg` - Retard applied while the launch is engaged. Under load a launch develops far more torque than a cruise at the same rpm, and launching at cruise timing is how the first clutch bite destroys a clutch.
- `launch_torque_scale` - Ceiling on the authority a launch may request.
- `launch_ramp_time_ms` - Time to reach the launch authority. A step from idle to full launch torque shakes the bike and is how a rider loses it; a ramp is a calibration, not a safety limit.
- `launch_timeout_ms` - Maximum time a launch may remain engaged. A launch control that never ends is a rev limiter with a worse personality.
- `launch_abort_torque_scale` - Authority on abort - clutch dump, brake applied, or the timeout. The abort path is what makes a launch a bounded feature rather than a condition.
- `launch_wheelie_mitigation_enabled` - Wheelie mitigation only if the hardware supports the required input. With no IMU and no fork-travel input there is nothing to mitigate with, and offering the feature anyway would be a fabricated capability.
- `launch_wheelie_front_lift_gain` - Reduction per unit of front-lift signal, when such a signal exists.
- `launch_fuel_trim_pct` - Small fuelling trim during a launch. Deliberately tiny: most launches want *less* mixture than cruise because of the heat soak, and a large trim is a flooding risk on a bike that is barely moving.
- `launch_min_speed_kph` - Road speed below which a launch is not permitted to engage.
- `launch_max_boost_kpa` - Boost ceiling during a launch. Drag starts can legitimately ask for more than street boost, and the safety limit still applies unchanged above this.

## `shift`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1000` | `shift_strategy` | enum | | fuel_cut | 0 | 2 | TUNER | HIGH | UNVERIFIED |
| `0x1001` | `shift_max_cut_ms` | u16 | ms | 120 | 5 | 2000 | TUNER | HIGH | UNVERIFIED |
| `0x1002` | `shift_min_time_between_ms` | u16 | ms | 400 | 50 | 1e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x1003` | `shift_recovery_torque_scale` | f32 | | 0.7 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1004` | `shift_blip_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x1005` | `shift_blip_duration_ms` | u16 | ms | 150 | 10 | 2000 | TUNER | MEDIUM | UNVERIFIED |
| `0x1006` | `shift_blip_torque_scale` | f32 | | 0.8 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x1007` | `shift_min_rpm` | u16 | rpm | 3000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x1008` | `shift_tps_threshold_pct` | f32 | % | 20 | 0 | 100 | TUNER | MEDIUM | UNVERIFIED |
| `0x1009` | `shift_release_timeout_ms` | u16 | ms | 1000 | 50 | 1e+04 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `shift_strategy` - What a quickshift actually removes. Fuel cut is the default because it is what a mechanical fuel-injected engine recovers from fastest; spark cut is gentler on the driveline but must never be allowed to overlap a traction cut.
- `shift_max_cut_ms` - Duration-bounded cut. The shift is a requester with a bounded lifetime, so a shift that never completes cannot leave the engine without fuel.
- `shift_min_time_between_ms` - Minimum time between two shifts. A bouncing switch or a noisy paddle must not be able to open a cut every cycle.
- `shift_recovery_torque_scale` - Authority during shift recovery. Ramped back rather than returned at once, so the re-engagement does not arrive as a second torque step.
- `shift_blip_enabled` - Auto-blip on downshift. Needs a clutch lever switch, which is not known to be fitted. Register UNK-GT-016.
- `shift_blip_torque_scale` - Authority during the blip. A blip is a throttle-matching event; giving it full authority is a clutch dump with extra steps.
- `shift_min_rpm` - Below this rpm a shift is ignored; there is nothing to gain.
- `shift_tps_threshold_pct` - Minimum throttle opening for a shift to count as deliberate. Coasting downshifts are a different event with different consequences.
- `shift_release_timeout_ms` - If the gear or clutch condition that ends the shift never arrives, this ends it. The re-enable predicate is bounded in time as well as in condition.

## `traction`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1100` | `traction_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1101` | `traction_slip_target_pct` | f32 | % | 8 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x1102` | `traction_slip_ramp_pct_per_s` | f32 | %/s | 20 | 0.1 | 500 | TUNER | HIGH | UNVERIFIED |
| `0x1103` | `traction_gear_correction` | f32 | | 1 | 0.1 | 3 | TUNER | MEDIUM | UNVERIFIED |
| `0x1104` | `traction_speed_correction_kph` | f32 | km/h | 40 | 0 | 400 | TUNER | MEDIUM | UNVERIFIED |
| `0x1105` | `traction_rpm_correction` | f32 | | 1 | 0.1 | 3 | TUNER | MEDIUM | UNVERIFIED |
| `0x1106` | `traction_min_slip_to_act` | f32 | % | 3 | 0 | 100 | TUNER | HIGH | UNVERIFIED |
| `0x1107` | `traction_intervention_hold_ms` | u16 | ms | 100 | 10 | 5000 | TUNER | HIGH | UNVERIFIED |
| `0x1108` | `traction_gain` | f32 | | 1 | 0 | 20 | TUNER | HIGH | UNVERIFIED |
| `0x1109` | `traction_retard_max_deg` | f32 | deg | 8 | 0 | 30 | TUNER | HIGH | UNVERIFIED |
| `0x110A` | `traction_min_speed_kph` | f32 | km/h | 5 | 0 | 200 | TUNER | MEDIUM | UNVERIFIED |
| `0x110B` | `traction_wheel_timeout_ms` | u16 | ms | 200 | 10 | 5000 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `traction_enabled` - Off by default: traction control needs front *and* rear wheel speed, and no ABS sensor is known to exist on this bike. Register UNK-GT-014. The GUI hides the workspace entirely when CAP_TRACTION is off.
- `traction_slip_target_pct` - Allowed slip ratio difference, percent, between driven and reference wheel. The number itself is a rider-preference compromise between fast and stable.
- `traction_slip_ramp_pct_per_s` - Slew rate on the target. A step target is a step torque demand, and a step torque demand on a rear tyre is a spin.
- `traction_gear_correction` - Multiplier on the slip target in first gear, where slip is physically larger and the target has to be looser to be stable.
- `traction_speed_correction_kph` - Speed at which the correction reaches full effect. Below it the target is tightened, because at walking pace a slip ratio is noise.
- `traction_rpm_correction` - Multiplier on the slip target with engine speed, to account for the extra inertia the rear wheel has to work with.
- `traction_min_slip_to_act` - Below this the controller does not act at all. Without it the loop chases sensor noise at zero slip and the rider feels an engine that is mysteriously hesitating.
- `traction_intervention_hold_ms` - Minimum time the control stays engaged after an intervention, so it does not chatter on and off as the tyre oscillates across the target.
- `traction_gain` - Authority per unit slip error; it becomes a torque-manager request.
- `traction_retard_max_deg` - Ceiling on the retard the traction requester may add.
- `traction_min_speed_kph` - Below this the controller is disabled. A two-wheel speed difference at 3 km/h is a speedometer discrepancy, not a slide.
- `traction_wheel_timeout_ms` - Freshness limit on the wheel-speed sources. A stale wheel reading is worse than none: the controller would act confidently on a value from half a second ago. Depends on: `traction_enabled`.

## `ebt`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1200` | `engine_braking_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x1201` | `ebt_retard_table` | table 8x8 f32 | | 0-filled | -12 | 12 | TUNER | HIGH | UNVERIFIED |
| `0x1202` | `ebt_fuel_cut_tps_pct` | f32 | % | 0.5 | 0 | 20 | TUNER | MEDIUM | UNVERIFIED |
| `0x1203` | `ebt_fuel_cut_map_kpa` | f32 | kPa | 35 | 0 | 150 | TUNER | MEDIUM | UNVERIFIED |
| `0x1204` | `ebt_min_rpm` | u16 | rpm | 2500 | 500 | 2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x1205` | `ebt_max_torque_scale` | f32 | | 0.3 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1206` | `ebt_recovery_tau_ms` | u16 | ms | 300 | 10 | 1e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x1207` | `ebt_retard_table_gear_axis` | axis1d | gear | 1 | 1 | 8 | TUNER | HIGH | UNVERIFIED |
| `0x1208` | `ebt_retard_table_rpm_axis` | axis1d | rpm | 1000 | 1000 | 1.2e+04 | TUNER | HIGH | UNVERIFIED |

**Notes**

- `engine_braking_enabled` - Off by default. On a motorcycle with twin carburettors the engine brake is usually the rider's own right foot; enabling a fuel-cut-based engine brake changes the bike's character permanently, so it is opt-in.
- `ebt_retard_table` - Retard on overrun against gear and rpm. Positive advances and negative retards, so the sign convention matches the rest of the ignition corrections and does not need its own explanation in a log.
- `ebt_fuel_cut_map_kpa` - Manifold pressure below which overrun fuel cut applies. As with the decel cut, this is the *engine braking* threshold, which is deliberately tighter than the ordinary overrun cut.
- `ebt_min_rpm` - Below this, engine braking is off: a lean mixture at low rpm is a stumble, not a brake, and a stumble under a closed throttle is how a bike gets dropped at a junction.
- `ebt_max_torque_scale` - Floor on the allowed scale while the engine brake is active. Bounded above zero so a mid-corner downshift cannot command a total loss of drive.
- `ebt_recovery_tau_ms` - Time constant for handing authority back on throttle return. Slow enough that releasing the throttle does not feel like a torque step.
- `ebt_retard_table_gear_axis` - gear breakpoints for ebt_retard_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.
- `ebt_retard_table_rpm_axis` - rpm breakpoints for ebt_retard_table: 8 of them, strictly increasing. These are the coordinates the table is interpolated along, and they live in the calibration rather than in the firmware so a tuner can put the resolution where the engine actually spends its time - at idle, usually, rather than spread evenly across a range it never visits.

## `pit`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1300` | `pit_limiter_enabled` | bool | | 0 | 0 | 1 | TUNER | HIGH | UNVERIFIED |
| `0x1301` | `pit_target_speed_kph` | f32 | km/h | 60 | 0 | 500 | TUNER | HIGH | UNVERIFIED |
| `0x1302` | `pit_target_rpm` | u16 | rpm | 4000 | 0 | 2e+04 | TUNER | HIGH | UNVERIFIED |
| `0x1303` | `pit_speed_hysteresis_kph` | f32 | km/h | 2 | 0 | 50 | TUNER | HIGH | UNVERIFIED |
| `0x1304` | `pit_rpm_hysteresis` | u16 | rpm | 200 | 0 | 3000 | TUNER | HIGH | UNVERIFIED |
| `0x1305` | `pit_gear_override_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x1306` | `pit_gear1_target_rpm` | u16 | rpm | 5000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x1307` | `pit_gear2_target_rpm` | u16 | rpm | 4000 | 0 | 2e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x1308` | `pit_release_delay_ms` | u16 | ms | 200 | 0 | 1e+04 | TUNER | HIGH | UNVERIFIED |
| `0x1309` | `pit_activation_switch_inverted` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `pit_limiter_enabled` - Off by default. A pit limiter that cannot be disengaged is a safety defect in itself, so the disengage path is defined and tested before the feature is offered at all (TQM-001 sec.5, test TM11).
- `pit_target_rpm` - Speed target needs a wheel-speed source, which this bike may not have; the rpm target works without one and is the safer of the two.
- `pit_speed_hysteresis_kph` - Hysteresis, so the limiter does not chatter on and off around the target as the bike accelerates through it in a straight line.
- `pit_gear_override_enabled` - Use per-gear rpm limits instead of the single rpm target. Useful on a circuit where the same engine speed means different things in different gears, but it depends on knowing the gear. Depends on: `gear_ratio_count`.
- `pit_release_delay_ms` - Minimum time between the cut ending and it being allowed to end again. The limiter owns its own cut, and this is the bound on that ownership.
- `pit_activation_switch_inverted` - True when the pit switch is active-low. The polarity is explicit because a pit limiter wired to the wrong state is a limiter that engages in the pit.

## `modes`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1400` | `modes_enabled` | bits[10] | | 0 | 0 | 1023 | MODE | MEDIUM | UNVERIFIED |
| `0x1401` | `mode_default_load` | enum | | street | 0 | 9 | MODE | MEDIUM | UNVERIFIED |
| `0x1402` | `mode_count` | u8 | | 7 | 1 | 10 | READ_ONLY | MEDIUM | UNVERIFIED |
| `0x1403` | `mode_switch_live` | bool | | 1 | 0 | 1 | MODE | HIGH | UNVERIFIED |
| `0x1404` | `mode_switch_coast_ms` | u16 | ms | 250 | 0 | 1e+04 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `modes_enabled` - Which drive modes exist in this configuration. A mode selects a calibration *overlay*; it never selects a code path, and it can never change a hard safety limit, because those live in the base image (CAL-001 sec.3).
- `mode_default_load` - Mode loaded on power-up, before the user chooses. Street is the default because it is the only mode that is safe to be in while the bike is being wheeled out of a workshop.
- `mode_count` - Number of modes this build declares. It is a build-time count, not a tuning choice, and it is why it cannot be changed while the engine runs. live_edit false.
- `mode_switch_live` - Whether a mode change may be applied while the engine is running. A mode switch is a small validated transaction that takes effect at a defined point in the cycle, never part way through an injection or a spark event.
- `mode_switch_coast_ms` - Minimum time between two mode changes. Modes select overlays, and a driver toggling between them mid-corner would otherwise get an overlay per cycle - a step change in fuelling and timing with no physical event behind it.

## `safety`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1500` | `rpm_limit` | u16 | rpm | 8000 | 2000 | 1.2e+04 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1501` | `rpm_hysteresis` | u16 | rpm | 300 | 0 | 3000 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1502` | `overspeed_rpm` | u16 | rpm | 1.05e+04 | 2000 | 2e+04 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1503` | `lean_lambda_limit` | f32 | lambda | 1.2 | 1 | 2 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1504` | `rich_lambda_limit` | f32 | lambda | 0.55 | 0.3 | 1.2 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1505` | `ect_max_c` | f32 | degC | 120 | 60 | 200 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1506` | `iat_max_c` | f32 | degC | 130 | 40 | 250 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1507` | `oil_pressure_min_bar` | f32 | bar | 1 | 0 | 10 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1508` | `oil_pressure_recovery_bar` | f32 | bar | 1.5 | 0 | 12 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1509` | `oil_temp_max_c` | f32 | degC | 130 | 60 | 200 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150A` | `egt_max_c` | f32 | degC | 900 | 200 | 1500 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150B` | `battery_min_v` | f32 | V | 9.5 | 4 | 14 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150C` | `battery_max_v` | f32 | V | 16 | 10 | 40 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150D` | `fuel_pressure_min_bar` | f32 | bar | 2 | 0 | 10 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150E` | `injector_duty_max_pct` | f32 | % | 80 | 5 | 100 | SAFETY | CRITICAL | UNVERIFIED |
| `0x150F` | `dwell_max` | f32 | ms | 8 | 1 | 20 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1510` | `boost_max_kpa` | f32 | kPa | 200 | 0 | 1000 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1511` | `sync_loss_action` | enum | | request_cut | 0 | 5 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1512` | `action_level` | enum | | retard | 0 | 5 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1513` | `oil_cut_timeout_ms` | u16 | ms | 5000 | 100 | 6e+04 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1514` | `lean_cut_hold_ms` | u16 | ms | 2000 | 50 | 3e+04 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1515` | `sensor_fallback_speed_limit_kph` | f32 | km/h | 60 | 0 | 300 | SAFETY | CRITICAL | UNVERIFIED |
| `0x1516` | `max_fuel_pressure_bar` | f32 | bar | 8 | 0 | 30 | SAFETY | CRITICAL | UNVERIFIED |

**Notes**

- `rpm_limit` - Hard rev limit. Above it there is no spark and no fuel. This is the *ECU* limit; the trigger subsystem's overspeed_period_us is a separate, faster and hardware-backed path for the case where the CPU is not keeping up (H-09). Depends on: `max_rpm`.
- `rpm_hysteresis` - Speed the limit releases at. Without hysteresis the limiter chatters on and off at the limit, and every cycle of that chatter is a fuelling transient.
- `overspeed_rpm` - Instantaneous overspeed, above which a fault is declared and outputs are inhibited without waiting for the limit. The gap between this and rpm_limit is the overshoot the limiter is allowed; it must exceed the measured scheduling error, which is not yet known (UNK-FW-002/003).
- `lean_lambda_limit` - Leanest lambda the engine may be commanded to run at. This cuts and limits; it does not merely warn. The commanded target and the long-term learner bias are both clamped by it, so a bad day cannot walk the mixture lean (test F17).
- `rich_lambda_limit` - Richest lambda permitted. A rich limit matters because over-fuelling is what destroys catalytic converters and what leaves a hot pipe that backfires on shutdown (H-05).
- `ect_max_c` - Coolant over-temperature limit. The action escalates retard, then limits, then cuts; the escalation ladder is a safety group decision, not a feature one, because a mode must not be able to soften it (H-07, TM12).
- `iat_max_c` - Charge-air temperature limit. Bound high because this is a *charge* sensor and genuinely hot air is survivable; it exists to catch a failed IAT that reports ambient forever and so disables every heat-based protection.
- `oil_pressure_min_bar` - Minimum oil pressure. A latching cut with recovery hysteresis, not a warning: a motorcycle that has lost oil pressure is a few seconds from a seized engine and a rider who does not know (H-08). Register UNK-GT-018.
- `oil_pressure_recovery_bar` - Pressure at which a latched oil cut is released. Above oil_pressure_min_bar by design: releasing at the trip point makes the ECU hunt across its own limit and the rider ride an engine that keeps cutting.
- `oil_temp_max_c` - Oil over-temperature limit; derates before coolant does on most engines.
- `egt_max_c` - Exhaust temperature limit. The single most consequential *unmeasured* number in this file: it is engine-, cylinder- and probe-specific, and it is the limit that stands between a lean condition and a melted exhaust valve. Register UNK-GT-006 for the coil and EGT probe placement is equally unknown.
- `battery_min_v` - Under-voltage limit. Below it the coils cannot reach primary current, so the honest response is to retard and derate rather than to keep commanding spark the hardware cannot deliver.
- `battery_max_v` - Over-voltage limit, from a failed regulator or a jump-start with the wrong battery. The ECU should refuse to run rather than let 18 V reach the analogue front end.
- `fuel_pressure_min_bar` - Minimum rail pressure. A pressure drop is compensated *and* limited: the compensation corrects the metering, the limit stops an engine that is being fed by a dying pump. Register UNK-GT-005.
- `injector_duty_max_pct` - ECU-side injector duty ceiling, independent of the hardware ceiling in hardware.injector_max_duty_pct. Two limits in two places is not redundancy for its own sake: the hardware one survives a wedged CPU, this one can be lowered by engineering mode per engine.
- `dwell_max` - Safety ceiling on commanded dwell. The hardware max-dwell timer is the independent enforcement that survives a hung CPU (H-03, test I3); this value must stay at or below it, and the validator refuses a configuration where it does not.
- `boost_max_kpa` - Non-bypassable overboost limit. A boost target above it is refused at validation time rather than clamped at runtime, so the mistake is visible in the tuner instead of hidden (H-19).
- `sync_loss_action` - What loss of crank synchronisation does. The default is a request to cut, because without phase the ECU cannot know which cylinder is on its exhaust stroke, and a fuelling or spark event at the wrong phase is exactly H-01. Values are ordered from "carry on and log" to "latched shutdown".
- `action_level` - The escalation ladder used when a limit is exceeded. The default is RETARD because a mild over-temperature or a single knock event should not stop a motorcycle mid-corner; the ladder exists so that persistence escalates automatically rather than relying on the rider noticing.
- `oil_cut_timeout_ms` - Maximum lifetime of the latching oil cut. Every latch has a bound: a latch with no release path is a slow brick in software form (TQM-001 R7).
- `lean_cut_hold_ms` - How long a lean excursion must persist before the cut latches. Short enough that a transient cannot be ridden through, long enough that a single noisy wideband frame does not stop the engine.
- `sensor_fallback_speed_limit_kph` - Road speed above which a load-sensor failure escalates from a limp to a reduced-authority state. The philosophy in SC-001 sec.4 is that losing a comfort function must not stop a motorcycle mid-corner - but it must also not be running full authority on a guess at 100 km/h.
- `max_fuel_pressure_bar` - Upper rail-pressure limit. A stuck regulator would otherwise push the injectors past every point of their flow curve and flood the engine.

## `comms`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0500` | `can_bitrate` | enum | | kbit_500 | 0 | 4 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0501` | `can_db_default_timeout_ms` | u16 | ms | 200 | 1 | 1e+04 | ENGINEER | HIGH | SOURCE-DERIVED CAN-001 sec.3 (timeout 200 ms example) |
| `0x0502` | `can_db_default_min_raw` | i32 | | 0 | -2.147e+09 | 2.147e+09 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0503` | `can_db_default_max_raw` | i32 | | 6.554e+04 | -2.147e+09 | 2.147e+09 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0504` | `can_db_auto_apply_on_connect` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0505` | `protocol_version` | u16 | | 1 | 1 | 6.554e+04 | READ_ONLY | HIGH | SOURCE-DERIVED PROTO-001 sec.1 (VER packed major/minor) |
| `0x0506` | `session_timeout_ms` | u32 | ms | 3e+04 | 1000 | 6e+05 | ENGINEER | HIGH | UNVERIFIED |
| `0x0507` | `session_command_budget` | u16 | | 1000 | 1 | 6.554e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0508` | `signed_tune_policy` | enum | | untrusted_engineering_override | 0 | 2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x0509` | `engineering_mode_enabled` | bool | | 1 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x050A` | `engineering_mode_timeout_ms` | u32 | ms | 3e+05 | 1000 | 3.6e+06 | ENGINEER | HIGH | UNVERIFIED |
| `0x050B` | `audit_log_enabled` | bool | | 1 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x050C` | `audit_log_entries` | u16 | | 256 | 16 | 6.554e+04 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x050D` | `wireless_enabled` | bool | | 0 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x050E` | `wifi_tx_power_dbm` | f32 | dBm | 19.5 | 0 | 20 | ENGINEER | MEDIUM | SOURCE-DERIVED ESP32-S3 datasheet (2.4 GHz 1T1R, up to +19.5 dBm) |
| `0x050F` | `wifi_channel` | u8 | | 6 | 1 | 13 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0510` | `wifi_max_clients` | u8 | | 2 | 1 | 8 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0511` | `wireless_connect_timeout_ms` | u32 | ms | 1.5e+04 | 1000 | 1.2e+05 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x0512` | `protocol_max_payload` | u16 | bytes | 4096 | 8 | 6.554e+04 | READ_ONLY | MEDIUM | UNVERIFIED |

**Notes**

- `can_bitrate` - Nominal CAN rate. Persisted per project. 500 kbit/s is the common choice on modern motorcycles; the exact controller count on the chosen part is still open (UNK-REF-005).
- `can_db_default_timeout_ms` - Default signal timeout applied to a new CAN database entry. A CAN frame may only be an input to the control loop if it has an explicit timeout, a fallback value and a validity flag - so the default is generous but finite.
- `can_db_default_min_raw` - Default raw validity window of a new signal. Wide by default, because a narrow default silently rejects a legitimate signal from an adapter with a different scaling.
- `can_db_auto_apply_on_connect` - Off by default: silently replacing a validated database on every connect makes it impossible to tell which database the engine is currently running against.
- `protocol_version` - Wire protocol version, major packed in the high byte. A minor bump is backward compatible; a major bump is negotiated and never guessed (PROTO-001 sec.1). Derived from the firmware build.
- `session_timeout_ms` - Bounded session lifetime. An expired session cannot write. Losing the session does not disturb the running engine, which is the behaviour that makes a dropped Wi-Fi link harmless (test PR8).
- `session_command_budget` - Commands a session may issue before re-authentication. The budget exists so that a stolen-but-unfinished session is bounded in time as well as in privilege.
- `signed_tune_policy` - How the ECU treats a tune with no valid RACCOON signature. This is a three-state trust model, and the middle state is a deliberate product decision, not a compromise: the release key signs firmware and *reference* configurations, but a third-party tuner who bought the product legitimately cannot obtain that key, so forcing every tune through it would make every legitimate tune permanently untrusted. Instead an unsigned tune is written only to the inactive slot, can never auto-activate, cannot arm until it is explicitly acknowledged with a valid session, and is labelled UNTRUSTED in every view *and in the log header* so a log can never misrepresent its own provenance. There is no public precedent for this, so the design has to stand on internal review alone (UNK-REF-006).
- `engineering_mode_enabled` - Whether engineering mode exists at all in this build. On by default, and conspicuously so: a build where it *cannot* be enabled is a build whose diagnostics and bench procedures are one mistake away from being unrecoverable. It still requires an authenticated ENGINEER credential and every action it permits is audited.
- `engineering_mode_timeout_ms` - Engineering mode expires on its own. It does not persist because someone enabled it once on a bench and never went back.
- `audit_log_enabled` - Signed audit log of every privileged action - who, what, when, outcome, plus a monotonic counter - surviving power cycles and tamper-evident. Required by SEC-001 sec.6 and test SEC-11.
- `audit_log_entries` - Retained audit entries. It is a ring: when it wraps, the fact that it wrapped is itself recorded, so a full log is never silently mistaken for a quiet one.
- `wireless_enabled` - The ESP32-S3 wireless transport. Off by default, and it should stay a workshop and paddock tool: it is a *transport*, not a privilege. If the radio is absent the ECU behaves identically, and Wi-Fi association latency is unmeasured (UNK-SW-007).
- `wifi_tx_power_dbm` - Transmit power. It is bounded rather than pinned at maximum: +20 dBm from a pocket of metal in a fairing is both a EMC liability and a battery cost (355 mA at full power).
- `wifi_channel` - Fixed 2.4 GHz channel. Fixed rather than scanned: a tuner that has to find the ECU on a congested band at a track is a tuner who cannot download a log before the session starts.
- `wifi_max_clients` - Simultaneous stations. Small on purpose: every client is a live path to a calibration write, and the number is not a performance knob.
- `wireless_connect_timeout_ms` - Association timeout before the transport is declared unavailable. Bounded so a dead AP cannot leave the transport layer spinning.
- `protocol_max_payload` - Per-transport payload ceiling: 4096 for USB, 8 for CAN, 64 for CAN-FD. A larger payload is fragmented with a bounded fragment count and times out. Derived from the transport.

## `logging`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x0600` | `log_trigger_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0601` | `log_trigger_rate_hz` | u16 | Hz | 2e+04 | 1 | 6e+04 | TUNER | MEDIUM | UNVERIFIED |
| `0x0602` | `log_fast_enabled` | bool | | 0 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0603` | `log_fast_rate_hz` | u16 | Hz | 1000 | 10 | 2e+04 | TUNER | MEDIUM | SOURCE-DERIVED LOG-001 sec.3 (FAST 1 kHz) |
| `0x0604` | `log_core_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0605` | `log_core_rate_hz` | u16 | Hz | 200 | 10 | 5000 | TUNER | MEDIUM | SOURCE-DERIVED LOG-001 sec.3 (CORE 200 Hz) |
| `0x0606` | `log_slow_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x0607` | `log_slow_rate_hz` | u16 | Hz | 20 | 1 | 1000 | TUNER | MEDIUM | SOURCE-DERIVED LOG-001 sec.3 (SLOW 20 Hz) |
| `0x0608` | `log_throughput_ceiling_kb_s` | u32 | kB/s | 2000 | 1 | 1e+05 | ENGINEER | HIGH | UNVERIFIED |
| `0x0609` | `log_ring_buffer_s` | u16 | s | 30 | 1 | 3600 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x060A` | `log_event_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x060B` | `log_trigger_log_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x060C` | `log_map_trace_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x060D` | `log_freeze_frame_enabled` | bool | | 1 | 0 | 1 | TUNER | MEDIUM | UNVERIFIED |
| `0x060E` | `log_channel_count_max` | u16 | | 120 | 1 | 1000 | ENGINEER | MEDIUM | UNVERIFIED |

**Notes**

- `log_trigger_enabled` - Per-tooth trigger log: tooth timestamps, periods, accepted flag, reason for rejection. This is the only rate that can show a decoder that is losing sync without the engine misfiring.
- `log_trigger_rate_hz` - Nominal ceiling on the trigger group's sample rate. The group is really per-tooth, so this is a bound for admission, not a request for a fixed rate.
- `log_fast_enabled` - Combustion-domain signals - knock envelope, spark current, crank angle.
- `log_core_enabled` - The main tuning dataset - rpm, tps, map, lambda, injection ms, ignition deg, dwell and the whole correction vector. On by default because this is the record most logs are actually wanted for.
- `log_throughput_ceiling_kb_s` - Total throughput ceiling across every group. A requested set that exceeds it is *refused with an explanation*, never silently degraded (test L7). The ceiling is meant to be measured against the real medium, which has not been selected yet - Register UNK-HW-009.
- `log_ring_buffer_s` - RAM ring that absorbs burst rates. If the medium cannot keep up, the oldest data is dropped and the drop is recorded in the file header - a log that is quietly truncated is worse than a short one, because it looks complete.
- `log_event_enabled` - Sparse event records - sync loss and gain, knock escalation, over-temperature, lean, overboost, rev limit, launch, shift, traction, sensor fault, watchdog, reset, brownout, calibration error. Pushed live as well as written, so a fault appears in the GUI *while it happens* rather than at the end of the session.
- `log_trigger_log_enabled` - The dense trigger log in its own format, retained in a RAM ring and drained to storage. It is what the trigger oscilloscope in the GUI renders with the decoder state overlaid.
- `log_map_trace_enabled` - Record the *active* table cell - id, row, col, value and interpolation fractions - with every sample group. It costs a few bytes per sample and it is the single most useful feature for answering "why did the engine do that" (LOG-001 sec.5, test L8).
- `log_freeze_frame_enabled` - On a DTC, snapshot a CORE plus SLOW frame and the trigger-log window around the event, and store it with the code. Without it a fault is only explicable by the rider's memory.
- `log_channel_count_max` - Upper bound on the channel count. Bounded at read time as well as at write time: a log file is fixed-size records with no unbounded channel count, so a malformed header cannot make a reader allocate.

## `strategy`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1800` | `injection_strategy` | enum | | semi_sequential | 0 | 3 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x1801` | `lambda_strategy` | enum | | off | 0 | 4 | ENGINEER | HIGH | UNVERIFIED |
| `0x1802` | `dbw_strategy` | enum | | off | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x1803` | `shift_request_source` | enum | | none | 0 | 3 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1804` | `lambda_channel_count` | u8 | | 0 | 0 | 4 | ENGINEER | HIGH | UNVERIFIED |
| `0x1805` | `knock_sensor_count` | u8 | | 0 | 0 | 2 | ENGINEER | HIGH | UNVERIFIED |
| `0x1806` | `wheel_speed_channel_count` | u8 | | 0 | 0 | 2 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1807` | `pedal_channel_count` | u8 | | 0 | 0 | 2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x1808` | `throttle_channel_count` | u8 | | 1 | 1 | 2 | ENGINEER | HIGH | UNVERIFIED |
| `0x1809` | `clutch_switch_present` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x180A` | `ignition_hard_min_deg` | f32 | deg | -10 | -40 | 20 | SAFETY | CRITICAL | UNVERIFIED |
| `0x180B` | `ignition_hard_max_deg` | f32 | deg | 35 | 0 | 60 | SAFETY | CRITICAL | UNVERIFIED |
| `0x180C` | `cam_phase_verified` | bool | | 0 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x180D` | `tdc_verified` | bool | | 0 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |

**Notes**

- `injection_strategy` - Injection firing mode. Sequential requires a valid cylinder phase source (cam sync, or a crank gap that fixes the 720 degree cycle); without one the ECU refuses to arm, because injecting into an exhaust stroke destroys an engine.
- `lambda_strategy` - Lambda strategy. Narrowband is explicitly NOT treated as a wide-range AFR measurement (FUEL_SPEC section 8); native wideband requires a physically present CJ125 plus LSU 4.9 pair (CAP_WIDEBAND).
- `dbw_strategy` - Electronic throttle. A cable throttle is NOT electronically controllable and must never be presented as DBW; without the pedal, motor and dual-channel redundancy hardware this stays 'off' (spec section 38, 44).
- `shift_request_source` - What tells the ECU a shift is wanted. Flat shift and auto-blip are impossible without a request source; the ECU does not invent one (spec section 43, 44).
- `lambda_channel_count` - Physically fitted lambda channels. Per-cylinder lambda is only claimed when a channel exists per cylinder; a single common-exhaust sensor is never demultiplexed into per-cylinder values without a validated methodology (FUEL_SPEC section 8, UNK-FW-009).
- `knock_sensor_count` - Physically fitted knock sensors. No sensor means no knock claim, ever (spec section 34).
- `wheel_speed_channel_count` - Fitted wheel speed channels. Traction control needs both axles to compute slip; one channel is enough for speed alone (spec section 45).
- `pedal_channel_count` - Pedal position channels. DBW requires two for redundancy.
- `throttle_channel_count` - Throttle position channels. Two are required for DBW plausibility; one is normal for a cable throttle (spec section 17).
- `clutch_switch_present` - A clutch lever switch is fitted. Used for launch, flat shift and rev matching (spec section 43).
- `ignition_hard_min_deg` - Lower bound of the hard ignition envelope. The final commanded advance is clamped into [ignition_hard_min_deg, ignition_hard_max_deg] AFTER every correction and every user trim, so no combination of trims can exceed it (spec section 31, CALIBRATION_SPEC section 110). Engineering mode may only narrow this envelope, never widen it.
- `ignition_hard_max_deg` - Upper bound of the hard ignition envelope. See ignition_hard_min_deg.
- `cam_phase_verified` - Set by the bench TDC procedure or the GUI TDC tool, never by the tuner, once the cam reference edge has been observed at a known TDC. The arming gate requires this for sequential injection and for the STROKE_DIRECTION item (spec section 98, GT650-001 section 4). Register UNK-GT-002, UNK-GT-003.
- `tdc_verified` - Set once the timing light agrees with the ECU's reported crank angle at a known RPM. Until this is set, the configuration is not armable: a wrong trigger angle is the single most damaging miscalibration in the product (spec section 98). Register UNK-GT-001.

## `switches`

| ID | Name | Type | Unit | Default | Min | Max | Owner | Safety | Source |
|---|---|---|---|---|---|---|---|---|---|
| `0x1900` | `kill_switch_present` | bool | | 0 | 0 | 1 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x1901` | `kill_switch_active_high` | bool | | 1 | 0 | 1 | ENGINEER | HIGH | UNVERIFIED |
| `0x1902` | `kill_switch_fault_behaviour` | enum | | inhibit_output | 0 | 2 | ENGINEER | CRITICAL | UNVERIFIED |
| `0x1903` | `brake_switch_present` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1904` | `brake_switch_active_high` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1905` | `side_stand_present` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1906` | `side_stand_active_high` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1907` | `launch_button_present` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1908` | `launch_button_active_high` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x1909` | `pit_switch_present` | bool | | 0 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x190A` | `pit_switch_active_high` | bool | | 1 | 0 | 1 | ENGINEER | MEDIUM | UNVERIFIED |
| `0x190B` | `switch_debounce_ms` | u16 | ms | 20 | 1 | 200 | TUNER | MEDIUM | UNVERIFIED |
| `0x190C` | `switch_timeout_ms` | u16 | ms | 5000 | 100 | 3e+04 | TUNER | MEDIUM | UNVERIFIED |

**Notes**

- `kill_switch_present` - A kill switch is fitted. Engine control must not arm while it is active.
- `kill_switch_active_high` - Which level means 'kill asserted'. A broken wire on a normally-closed kill circuit must present as 'engine may run', so the polarity is an explicit calibration rather than a default guess (SENSOR_SPEC section 5).
- `kill_switch_fault_behaviour` - What a broken or missing kill-switch signal does. 'ignore' is refused by the schema rules: a build that cannot tell whether the rider asked to stop is unsafe.
- `brake_switch_present` - A brake input is fitted. Used as a traction-control reference and to inhibit rev matching and the pit limiter (spec section 45, 47).
- `brake_switch_active_high` - Which level means 'brake applied'.
- `side_stand_present` - A side-stand switch is fitted. Usually retained by the OEM ECU; the RACCOON input exists so the tuner can see its state in logs and diagnostics.
- `side_stand_active_high` - Which level means 'stand down'.
- `launch_button_present` - A launch button is fitted. Without one, launch control has no request source.
- `launch_button_active_high` - Which level means 'launch requested'.
- `pit_switch_present` - A pit-lane switch is fitted (spec section 47).
- `pit_switch_active_high` - Which level means 'pit limiter armed'.
- `switch_debounce_ms` - Shared debounce window for the switch inputs. A real filter with a bounded window, not a single sample (SENSOR_SPEC section 5).
- `switch_timeout_ms` - No-transition timeout for switch inputs. A signal that never changes for this long is reported stale; it is never silently treated as valid.

