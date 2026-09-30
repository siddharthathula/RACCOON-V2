/* RACCOON V2 - sensor framework tests: the fault taxonomy and the validity rules. */
#include "rc_test.h"

#include "rc/sensors.h"

static rc_sensors_t sensors;

static uint8_t add_linear(float slope, float offset, float lo, float hi, float rate)
{
    rc_sensor_config_t config;
    memset(&config, 0, sizeof(config));
    config.slope = slope;
    config.offset = offset;
    config.min = lo;
    config.max = hi;
    config.max_rate = rate;
    config.freshness_us = 20000u;
    config.frozen_window_us = 5000u;
    config.behaviour = (uint8_t)RC_SENSOR_FB_SUBSTITUTE;
    config.enabled = 1u;
    return rc_sensors_add(&sensors, &config);
}

/** ADC counts for an engineering value, clamped to what a 12-bit ADC can actually hold.
 *
 *  The open/short rail check divides the raw count by the full-scale count, so a count
 *  beyond full scale is correctly diagnosed as a short to the high rail.  A fixture that
 *  ignores the ADC width therefore does not test what it thinks it tests - it tests the
 *  rail detector.  Clamping keeps each case about the behaviour it names. */
#define TEST_ADC_FULL_SCALE 4095.0f

static uint16_t raw_for(float engineering, float slope, float offset)
{
    const float counts = ((engineering - offset) / slope) + 0.5f;
    if (counts < 0.0f) {
        return 0u;
    }
    if (counts > TEST_ADC_FULL_SCALE) {
        return (uint16_t)TEST_ADC_FULL_SCALE;
    }
    return (uint16_t)counts;
}

static void open_and_short_are_structural(void)
{
    rc_sensors_init(&sensors);
    const float slope = 0.01f; /* 40.95 V full scale: 0..5 V mapped to 0..40.95 */
    const uint8_t ch = add_linear(slope, 0.0f, -5.0f, 45.0f, 10000.0f);
    RC_CHECK_EQ_U(ch, 0u);
    RC_CHECK_EQ_U(rc_sensors_count(&sensors), 1u);

    /* Near ground: open circuit. */
    rc_sensors_update(&sensors, ch, 5u, 1000u, 1u);
    RC_CHECK((sensors.channel[ch].value.fault & RC_SENSOR_F_OPEN) != 0u);
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ch]));
    RC_CHECK_MSG(strcmp(rc_sensors_fault_text(sensors.channel[ch].value.fault),
                        "open circuit") == 0,
                 "got '%s'", rc_sensors_fault_text(sensors.channel[ch].value.fault));

    /* Near the rail: short circuit. */
    rc_sensors_update(&sensors, ch, 4090u, 2000u, 2u);
    RC_CHECK((sensors.channel[ch].value.fault & RC_SENSOR_F_SHORT) != 0u);
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ch]));

    /* Mid-scale: valid. */
    rc_sensors_update(&sensors, ch, raw_for(2.5f, slope, 0.0f), 3000u, 3u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[ch]));
    RC_CHECK_NEAR(sensors.channel[ch].value.value, 2.5f, 0.01f);
    RC_CHECK_EQ_U(sensors.channel[ch].value.fault, 0u);
}

static void out_of_range_is_refused_not_clamped(void)
{
    rc_sensors_init(&sensors);
    const float slope = 0.01f;
    /* The configured range is narrower than the electrical range, so a plausible-looking
     * reading outside it must be refused, not clipped into a valid-looking number. */
    const uint8_t ch = add_linear(slope, 0.0f, 0.0f, 20.0f, 10000.0f);
    rc_sensors_update(&sensors, ch, raw_for(30.0f, slope, 0.0f), 1000u, 1u);
    RC_CHECK((sensors.channel[ch].value.fault & RC_SENSOR_F_RANGE) != 0u);
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ch]));
    /* The fallback is used downstream, and it is visible. */
    RC_CHECK(sensors.channel[ch].value.using_fallback != 0u);
    RC_CHECK_NEAR(rc_sensors_value(&sensors.channel[ch]), 0.0f, 1e-6);
}

static void stale_and_frozen_are_distinct(void)
{
    rc_sensors_init(&sensors);
    const float slope = 0.01f;
    const uint8_t ch = add_linear(slope, 0.0f, 0.0f, 50.0f, 1000.0f);

    /* The frame pipeline stalls: the sequence does not advance. */
    rc_sensors_update(&sensors, ch, raw_for(2.0f, slope, 0.0f), 1000u, 7u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[ch]));
    rc_sensors_update(&sensors, ch, raw_for(2.0f, slope, 0.0f), 2000u, 7u);
    RC_CHECK_MSG((sensors.channel[ch].value.fault & RC_SENSOR_F_STALE) != 0u,
                 "a repeated sequence must be reported as stale");
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ch]));

    /* Frozen: the sequence advances but the value never moves, for a long time. */
    rc_sensors_init(&sensors);
    const uint8_t frozen = add_linear(slope, 0.0f, 0.0f, 50.0f, 100000.0f);
    rc_sensors_update(&sensors, frozen, raw_for(2.0f, slope, 0.0f), 1000u, 1u);
    for (uint32_t seq = 2u; seq < 12u; ++seq) {
        rc_sensors_update(&sensors, frozen, raw_for(2.0f, slope, 0.0f), 1000u + (seq * 1000u), seq);
    }
    RC_CHECK_MSG((sensors.channel[frozen].value.fault & RC_SENSOR_F_FROZEN) != 0u,
                 "an unchanging value with an advancing sequence must be reported as frozen");
    /* A real change clears it. */
    rc_sensors_update(&sensors, frozen, raw_for(3.0f, slope, 0.0f), 20000u, 20u);
    RC_CHECK((sensors.channel[frozen].value.fault & RC_SENSOR_F_FROZEN) == 0u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[frozen]));
}

static void rate_limit_is_flagged_but_passes_the_value(void)
{
    rc_sensors_init(&sensors);
    const float slope = 0.01f;
    const uint8_t ch = add_linear(slope, 0.0f, 0.0f, 50.0f, 10.0f); /* 10 units/second */
    rc_sensors_update(&sensors, ch, raw_for(1.0f, slope, 0.0f), 1000u, 1u);
    /* A jump of 20 units in 1 ms is physically impossible for this sensor. */
    rc_sensors_update(&sensors, ch, raw_for(21.0f, slope, 0.0f), 2000u, 2u);
    RC_CHECK_MSG((sensors.channel[ch].value.fault & RC_SENSOR_F_RATE) != 0u,
                 "an impossible rate must be flagged");
    /* The value is still passed through: a real transient must stay visible in a log. */
    RC_CHECK_NEAR(sensors.channel[ch].value.value, 21.0f, 0.01f);
    /* A plausible rate is not flagged. */
    rc_sensors_update(&sensors, ch, raw_for(21.01f, slope, 0.0f), 3000u, 3u);
    RC_CHECK((sensors.channel[ch].value.fault & RC_SENSOR_F_RATE) == 0u);
}

static void reference_rail_fault_invalidates_dependents(void)
{
    rc_sensors_init(&sensors);
    const float slope = 0.01f;
    const uint8_t ref = add_linear(slope, 0.0f, 4.5f, 5.5f, 1000.0f);
    const uint8_t tps = add_linear(slope, 0.0f, 0.0f, 100.0f, 10000.0f);
    /* A 0-300 kPa sensor needs a slope of ~0.073 to stay inside a 12-bit ADC. */
    const float map_slope = 300.0f / 4000.0f;
    const uint8_t map = add_linear(map_slope, 0.0f, 0.0f, 300.0f, 10000.0f);
    rc_sensors_update(&sensors, ref, raw_for(5.0f, slope, 0.0f), 1000u, 1u);
    rc_sensors_update(&sensors, tps, raw_for(10.0f, slope, 0.0f), 1000u, 1u);
    rc_sensors_update(&sensors, map, raw_for(100.0f, map_slope, 0.0f), 1000u, 1u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[tps]));
    RC_CHECK(rc_sensors_usable(&sensors.channel[map]));

    /* The 5 V rail collapses.  Every 5 V sensor would otherwise keep reporting a
     * plausible-looking number computed from a broken front end. */
    rc_sensors_reference_fault(&sensors, ref, true);
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ref]));
    RC_CHECK_MSG(!rc_sensors_usable(&sensors.channel[tps]),
                 "a channel fed by the failed rail must be invalidated");
    RC_CHECK(!rc_sensors_usable(&sensors.channel[map]));
    RC_CHECK((sensors.channel[tps].value.fault & RC_SENSOR_F_REFERENCE) != 0u);

    /* Recovery: the rail returns and the channels come back without an ECU reset. */
    rc_sensors_reference_fault(&sensors, ref, false);
    rc_sensors_update(&sensors, ref, raw_for(5.0f, slope, 0.0f), 2000u, 2u);
    rc_sensors_update(&sensors, tps, raw_for(10.0f, slope, 0.0f), 2000u, 2u);
    rc_sensors_update(&sensors, map, raw_for(101.0f, map_slope, 0.0f), 2000u, 2u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[tps]));
    RC_CHECK(rc_sensors_usable(&sensors.channel[map]));
}

static void cross_check_is_diagnostic_not_structural(void)
{
    rc_sensors_init(&sensors);
    /* A slope of 0.02 puts 45 V at 2250 counts - comfortably inside a 12-bit ADC and far
     * from either rail.  At 0.01 the 45 V sample would sit at full scale and be correctly
     * diagnosed as a short to the high rail, so the cross check would never run. */
    const float slope = 0.02f;
    const uint8_t a = add_linear(slope, 0.0f, 0.0f, 100.0f, 10000.0f);
    const uint8_t b = add_linear(slope, 0.0f, 0.0f, 100.0f, 10000.0f);
    rc_sensors_update(&sensors, a, raw_for(20.0f, slope, 0.0f), 1000u, 1u);
    rc_sensors_update(&sensors, b, raw_for(20.0f, slope, 0.0f), 1000u, 1u);
    rc_sensors_cross_check(&sensors, a, b, 2.0f);
    RC_CHECK((sensors.channel[a].value.fault & RC_SENSOR_F_INCONSISTENT) == 0u);

    /* Channel a must present a NEW value: repeating the previous reading is a frozen
     * signal, which correctly makes the channel unusable and would mean the cross check
     * never got the chance to disagree. */
    rc_sensors_update(&sensors, a, raw_for(20.5f, slope, 0.0f), 2000u, 2u);
    rc_sensors_update(&sensors, b, raw_for(45.0f, slope, 0.0f), 2000u, 2u);
    rc_sensors_cross_check(&sensors, a, b, 2.0f);
    RC_CHECK((sensors.channel[a].value.fault & RC_SENSOR_F_INCONSISTENT) != 0u);
    /* Plausibility, not validity: a dual-channel disagreement is information for the
     * tuner, not proof that either channel is broken, so the value stays usable. */
    RC_CHECK_MSG(rc_sensors_usable(&sensors.channel[a]),
                 "an implausible pair must not make the channel unusable");
    RC_CHECK_EQ_U(sensors.channel[a].value.plausible, 0u);

    /* Agreement clears it. */
    rc_sensors_update(&sensors, a, raw_for(45.0f, slope, 0.0f), 3000u, 3u);
    rc_sensors_cross_check(&sensors, a, b, 2.0f);
    RC_CHECK((sensors.channel[a].value.fault & RC_SENSOR_F_INCONSISTENT) == 0u);
    RC_CHECK_EQ_U(sensors.channel[a].value.plausible, 1u);
}

static void fault_injection_marks_channels(void)
{
    rc_sensors_init(&sensors);
    const uint8_t ch = add_linear(0.01f, 0.0f, 0.0f, 50.0f, 10000.0f);
    rc_sensors_update(&sensors, ch, raw_for(2.0f, 0.01f, 0.0f), 1000u, 1u);
    RC_CHECK(rc_sensors_usable(&sensors.channel[ch]));
    rc_sensors_invalidate(&sensors, ch, RC_SENSOR_F_DMA);
    RC_CHECK(!rc_sensors_usable(&sensors.channel[ch]));
    RC_CHECK(strcmp(rc_sensors_fault_text(RC_SENSOR_F_DMA), "DMA fault") == 0);
    /* An out-of-range index must be ignored, not corrupt channel 0. */
    rc_sensors_invalidate(&sensors, 200u, RC_SENSOR_F_ADC);
    RC_CHECK(rc_sensors_usable(&sensors.channel[ch]) == false);
}

void test_sensors(void)
{
    RC_TEST_CASE("open/and/short/are/structural", open_and_short_are_structural);
    RC_TEST_CASE("out/of/range/is/refused/not/clamped", out_of_range_is_refused_not_clamped);
    RC_TEST_CASE("stale/and/frozen/are/distinct", stale_and_frozen_are_distinct);
    RC_TEST_CASE("rate/limit/is/flagged/but/passes/the/value", rate_limit_is_flagged_but_passes_the_value);
    RC_TEST_CASE("reference/rail/fault/invalidates/dependents", reference_rail_fault_invalidates_dependents);
    RC_TEST_CASE("cross/check/is/diagnostic/not/structural", cross_check_is_diagnostic_not_structural);
    RC_TEST_CASE("fault/injection/marks/channels", fault_injection_marks_channels);
}
