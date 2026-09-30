/* RACCOON V2 - math and CRC tests. */
#include "rc_test.h"

#include <math.h>

#include "rc/rc_crc32.h"
#include "rc/rc_math.h"

static void math_bounds(void)
{
    RC_CHECK_NEAR(rc_clampf(5.0f, 0.0f, 10.0f), 5.0f, 0.0);
    RC_CHECK_NEAR(rc_clampf(-1.0f, 0.0f, 10.0f), 0.0f, 0.0);
    RC_CHECK_NEAR(rc_clampf(99.0f, 0.0f, 10.0f), 10.0f, 0.0);
    /* NaN must fall to the lower bound, never escape upward: a poisoned input that
     * returns hi is how a NaN turns into full torque. */
    RC_CHECK_NEAR(rc_clampf(NAN, 3.0f, 10.0f), 3.0f, 0.0);
}

static void math_angles(void)
{
    RC_CHECK_NEAR(rc_wrapf(-10.0f, 360.0f), 350.0f, 1e-4);
    RC_CHECK_NEAR(rc_wrapf(370.0f, 360.0f), 10.0f, 1e-4);
    RC_CHECK_NEAR(rc_wrapf(720.0f, 360.0f), 0.0f, 1e-4);
    /* Shortest way round, which is what a scheduler offset needs. */
    RC_CHECK_NEAR(rc_angle_delta(350.0f, 10.0f, 360.0f), 20.0f, 1e-4);
    RC_CHECK_NEAR(rc_angle_delta(10.0f, 350.0f, 360.0f), -20.0f, 1e-4);
    RC_CHECK_NEAR(rc_angle_delta(0.0f, 180.0f, 360.0f), 180.0f, 1e-4);
}

static void math_tables(void)
{
    /* A 2x2 table with a known bilinear answer, checked including the clamped edges -
     * extrapolation is forbidden, so an out-of-range coordinate must return the edge. */
    const float table[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 0.0f, 0.0f, 1.0f), 10.0f, 1e-4);
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 0.0f, 1.0f, 1.0f), 20.0f, 1e-4);
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 1.0f, 0.0f, 1.0f), 30.0f, 1e-4);
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 1.0f, 1.0f, 1.0f), 40.0f, 1e-4);
    /* The exact midpoint of a planar 2x2 is the mean of the four corners. */
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 0.5f, 0.5f, 1.0f), 25.0f, 1e-4);
    /* Clamping, not extrapolation. */
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, 5.0f, 5.0f, 1.0f), 40.0f, 1e-4);
    RC_CHECK_NEAR(rc_table2d_interp(table, RC_CELL_F32, 2, 2, -5.0f, -5.0f, 1.0f), 10.0f, 1e-4);

    /* A u8 table must be read as u8, not as float: this is the misinterpretation that the
     * explicit cell kind exists to prevent. */
    const uint8_t cells[4] = {0u, 100u, 200u, 255u};
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_U8, 2, 2, 0.0f, 1.0f, 1.0f), 100.0f, 1e-4);
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_U8, 2, 2, 1.0f, 1.0f, 1.0f), 255.0f, 1e-4);
    /* ...and a 0..200 percent VE scale comes out in percent. */
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_U8, 2, 2, 1.0f, 0.0f, 200.0f / 255.0f),
                  (200.0 / 255.0) * 200.0, 1e-3);
}

static void math_thermistor(void)
{
    /* Steinhart-Hart must return the reference temperature at the reference resistance,
     * to within a millikelvin: that single identity pins the whole equation. */
    const float celsius = rc_steinhart_hart(10000.0f, 3950.0f, 10000.0f, 298.15f);
    RC_CHECK_NEAR(celsius, 25.0f, 0.001);
    /* Monotonic: a hotter sensor (lower resistance in an NTC) reads higher. */
    RC_CHECK(rc_steinhart_hart(5000.0f, 3950.0f, 10000.0f, 298.15f) > celsius);
    RC_CHECK(rc_steinhart_hart(20000.0f, 3950.0f, 10000.0f, 298.15f) < celsius);
}

static void math_rate_limit(void)
{
    bool limited = false;
    /* A hard bound on how fast a value may move, with the flag visible. */
    RC_CHECK_NEAR(rc_rate_limit(0.0f, 100.0f, 10.0f, &limited), 10.0f, 1e-4);
    RC_CHECK(limited);
    RC_CHECK_NEAR(rc_rate_limit(0.0f, 5.0f, 10.0f, &limited), 5.0f, 1e-4);
    RC_CHECK(!limited);
    RC_CHECK_NEAR(rc_rate_limit(0.0f, -100.0f, 10.0f, &limited), -10.0f, 1e-4);
    RC_CHECK(limited);
}

static void math_decay(void)
{
    /* An exponential decay: after one time constant a third of the step remains. */
    const float after = rc_decay(0.0f, 1.0f, 0.001f, 0.001f);
    RC_CHECK_NEAR(after, 0.632, 0.005);
    RC_CHECK_NEAR(rc_decay(1.0f, 0.0f, 0.0f, 1.0f), 1.0f, 1e-6);
    /* A zero time constant means "instant", not "divide by zero". */
    RC_CHECK_NEAR(rc_decay(5.0f, 9.0f, 1.0f, 0.0f), 9.0f, 1e-6);
}

static void math_safe_div(void)
{
    RC_CHECK_NEAR(rc_safe_div(10.0f, 2.0f, -1.0f), 5.0f, 1e-6);
    RC_CHECK_NEAR(rc_safe_div(10.0f, 0.0f, -1.0f), -1.0f, 1e-6);
    RC_CHECK_NEAR(rc_safe_div(10.0f, 1e-12f, -7.0f), -7.0f, 1e-6);
}

static void crc_vectors(void)
{
    /* Known vectors. CRC-32C of "123456789" is 0xE3069283; CRC-32 (IEEE) is 0xCBF43926.
     * If the table generation is ever wrong, these fail and nothing else matters. */
    const char *text = "123456789";
    const size_t len = strlen(text);
    RC_CHECK_EQ_U(rc_crc32c(0u, text, len), 0xE3069283u);
    RC_CHECK_EQ_U(rc_crc32(0u, text, len), 0xCBF43926u);
    /* The empty string and a null buffer are both no-ops, not crashes. */
    RC_CHECK_EQ_U(rc_crc32c(0u, "", 0u), 0u);
    RC_CHECK_EQ_U(rc_crc32c(0u, NULL, 8u), 0u);
    /* Incremental equals one-shot. */
    const uint32_t split = rc_crc32c(0u, text, 4u);
    RC_CHECK_EQ_U(rc_crc32c(split, text + 4, len - 4u), 0xE3069283u);
}

static void crc_digest(void)
{
    rc_digest_t digest;
    rc_digest_init(&digest);
    rc_digest_update(&digest, "12345", 5u);
    rc_digest_update(&digest, "6789", 4u);
    RC_CHECK_EQ_U(rc_digest_final(&digest), 0xE3069283u);
    RC_CHECK_EQ_U(digest.length, 9u);
    /* A single flipped bit must change the digest: this is what catches a scrambled
     * calibration image, and a CRC with a 1-bit blind spot would not catch it. */
    uint8_t data[9];
    memcpy(data, "123456789", 9u);
    rc_digest_init(&digest);
    rc_digest_update(&digest, data, 9u);
    const uint32_t before = rc_digest_final(&digest);
    data[4] = (uint8_t)(data[4] ^ 0x01u);
    rc_digest_init(&digest);
    rc_digest_update(&digest, data, 9u);
    RC_CHECK(rc_digest_final(&digest) != before);
}

/* An axis is only worth storing if it changes the answer. These tests use a table that is a
 * known ramp and deliberately lumpy axes, so every expected value can be worked out by hand
 * from the breakpoints. A test built from the same arithmetic as the code proves only that
 * the code agrees with itself.
 *
 * The ramp is 1 2 3 / 4 5 6 / 7 8 9 as f32, so cell (r, c) is 1 + 3r + c, and the first
 * column down each row is 1, 4, 7. */
static void axis_lookup_finds_the_right_cell(void)
{
    const uint16_t axis[5] = {1000u, 1200u, 1400u, 3000u, 12000u};
    uint8_t lo = 0u;
    uint8_t hi = 0u;
    float frac = 0.0f;

    /* Inside the dense low end: 1,100 is halfway between 1,000 and 1,200. */
    RC_CHECK(rc_axis_locate(axis, RC_CELL_U16, 5u, 1100.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 0u);
    RC_CHECK_EQ_U(hi, 1u);
    RC_CHECK_NEAR(frac, 0.5f, 1e-4f);

    /* Inside the sparse high end: 7,000 is (7000-3000)/9000 = 0.444 of the way along a bin
     * 9,000 rpm wide, which is what makes one breakpoint worth 4,500 rpm up here and 200
     * rpm down at the bottom. */
    RC_CHECK(rc_axis_locate(axis, RC_CELL_U16, 5u, 7000.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 3u);
    RC_CHECK_EQ_U(hi, 4u);
    RC_CHECK_NEAR(frac, 4000.0f / 9000.0f, 1e-4f);

    /* On a breakpoint exactly, the fraction is 0 rather than an interpolation toward the
     * NEXT pair. Getting this wrong puts every lookup on the wrong cell at every bin edge,
     * which is most of the range. */
    RC_CHECK(rc_axis_locate(axis, RC_CELL_U16, 5u, 1400.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 2u);
    RC_CHECK_EQ_U(hi, 3u);
    RC_CHECK_NEAR(frac, 0.0f, 1e-6f);

    /* Outside: clamped to the end, never extrapolated. The ECU inventing fuel or spark past
     * the last breakpoint a tuner set is not a small error. */
    RC_CHECK(rc_axis_locate(axis, RC_CELL_U16, 5u, 0.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 0u);
    RC_CHECK_EQ_U(hi, 0u);
    RC_CHECK_NEAR(frac, 0.0f, 1e-6f);
    RC_CHECK(rc_axis_locate(axis, RC_CELL_U16, 5u, 99999.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 4u);
    RC_CHECK_EQ_U(hi, 4u);
    RC_CHECK_NEAR(frac, 0.0f, 1e-6f);

    /* NaN has no position on any axis. */
    RC_CHECK(!rc_axis_locate(axis, RC_CELL_U16, 5u, NAN, &lo, &hi, &frac));
}

/* What rc_axis_locate does and does not promise, pinned down.
 *
 * It is a SEARCH, not a validator, and the difference is load-bearing. A binary search over
 * an array that is only partly ordered converges on the sorted prefix and returns a
 * well-formed answer from it: 2,500 rpm against {1000, 3000, 2000} lands cleanly on the
 * (1000, 3000) pair and returns a number, even though that axis is nonsense.
 *
 * So this function cannot be relied on to catch a broken axis, and these tests do not claim
 * it does. The guarantee that an axis is monotonic comes from rc_cal_axis_is_monotonic(),
 * which examines EVERY pair and runs at activation time - see tests/unit/test_calibration.c.
 * A calibration with a bad axis never reaches a live lookup. */
static void axis_lookup_is_a_search_not_a_validator(void)
{
    uint8_t lo = 0u;
    uint8_t hi = 0u;
    float frac = 0.0f;

    /* A partly-ordered axis still returns a number, from the part that is ordered. Here the
     * first two breakpoints are in order and the third is not, and the search converges on
     * the ordered pair: 1,500 rpm brackets (1000, 3000) and reads a clean 0.25, even though
     * the axis as a whole is nonsense. */
    const uint16_t partly_sorted[3] = {1000u, 3000u, 2000u};
    RC_CHECK(rc_axis_locate(partly_sorted, RC_CELL_U16, 3u, 1500.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 0u);
    RC_CHECK_EQ_U(hi, 1u);
    RC_CHECK_NEAR(frac, 0.25f, 1e-4f);

    /* The same array read at 2,500 rpm. The axis maximum is read from the LAST element, which
     * the inversion has dragged down to 2,000 - so 2,500 rpm is "above the axis" and clamps,
     * rather than being searched at all. Another reason this function cannot be trusted to
     * police an axis: the damage the inversion does is visible as a silently wrong ceiling. */
    RC_CHECK(rc_axis_locate(partly_sorted, RC_CELL_U16, 3u, 2500.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 2u);
    RC_CHECK_EQ_U(hi, 2u);
    RC_CHECK_NEAR(frac, 0.0f, 1e-6f);

    /* A degenerate pair sitting at the very END of the axis is never reached, because the
     * clamp fires first: 2,000 is the axis maximum, so 2,500 rpm is simply the last cell.
     * 1,500 rpm is still a clean answer from the (1000, 2000) bin, and must stay one -
     * refusing it would break any axis with a flat spot below the operating point. */
    const uint16_t flat_tail[3] = {1000u, 2000u, 2000u};
    RC_CHECK(rc_axis_locate(flat_tail, RC_CELL_U16, 3u, 1500.0f, &lo, &hi, &frac));
    RC_CHECK_NEAR(frac, 0.5f, 1e-4f);
    RC_CHECK(rc_axis_locate(flat_tail, RC_CELL_U16, 3u, 2500.0f, &lo, &hi, &frac));
    RC_CHECK_EQ_U(lo, 2u);
    RC_CHECK_NEAR(frac, 0.0f, 1e-6f);

    /* Below two breakpoints there is nothing to interpolate between at all. */
    const uint16_t axis[5] = {1000u, 1200u, 1400u, 3000u, 12000u};
    RC_CHECK(!rc_axis_locate(axis, RC_CELL_U16, 1u, 1500.0f, &lo, &hi, &frac));
    RC_CHECK(!rc_axis_locate(axis, RC_CELL_U16, 0u, 1500.0f, &lo, &hi, &frac));
    RC_CHECK(!rc_axis_locate(NULL, RC_CELL_U16, 5u, 1500.0f, &lo, &hi, &frac));
}

/* The same ramp read under two different axes. If the axis were being ignored, `on_even` and
 * `on_lumpy` would come out equal and the first assertion would fail. */
static void a_lumpy_axis_really_does_change_the_answer(void)
{
    const float cells[9] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
    const uint16_t even[3] = {1000u, 6500u, 12000u};
    /* Dense low, sparse high: the shape a tuner dials in for an engine that lives at idle. */
    const uint16_t lumpy[3] = {1000u, 2000u, 12000u};
    const uint16_t columns[3] = {0u, 1u, 2u};

    /* 5,000 rpm. On `even` it is still in the FIRST bin, at (5000-1000)/5500 = 0.727, so it
     * reads 1 + 3*0.727 = 3.18. On `lumpy` it has already moved into the SECOND bin, at
     * (5000-2000)/10000 = 0.3, so it reads 4 + 3*0.3 = 4.9. Same rpm, same table, a cell and
     * a half apart. */
    const float on_even = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, even, columns,
                                               RC_CELL_U16, 5000.0f, 0.0f, 1.0f);
    const float on_lumpy = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                                RC_CELL_U16, 5000.0f, 0.0f, 1.0f);
    RC_CHECK_NEAR(on_even, 3.1818f, 0.01f);
    RC_CHECK_NEAR(on_lumpy, 4.9f, 0.01f);
    RC_CHECK_MSG(fabsf(on_even - on_lumpy) > 0.5f,
                 "a different axis produced the same value, so the axis is being ignored");

    /* 1,500 rpm is in the first bin on BOTH axes, but at different fractions of it: 1.27
     * under `even`, 2.5 under `lumpy`. Two axes, one rpm, two answers. */
    const float low_even = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, even, columns,
                                                RC_CELL_U16, 1500.0f, 0.0f, 1.0f);
    const float low_lumpy = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                                 RC_CELL_U16, 1500.0f, 0.0f, 1.0f);
    RC_CHECK_NEAR(low_even, 1.2727f, 0.01f);
    RC_CHECK_NEAR(low_lumpy, 2.5f, 0.01f);

    /* 6,000 rpm is still in `lumpy`'s second bin, at (6000-2000)/10000 = 0.4: 4 + 3*0.4. */
    const float at_six = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                              RC_CELL_U16, 6000.0f, 0.0f, 1.0f);
    RC_CHECK_NEAR(at_six, 5.2f, 0.01f);

    /* 12,000 rpm is the top breakpoint: the last row, with no interpolation past it. */
    const float at_top = rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                              RC_CELL_U16, 12000.0f, 0.0f, 1.0f);
    RC_CHECK_NEAR(at_top, 7.0f, 0.01f);

    /* 1,000 rpm is the bottom breakpoint, so it pins row 0 and the column coordinate is the
     * only thing left: the ramp's top row reads 1, 2, 3 across columns 0, 1, 2. */
    RC_CHECK_NEAR(rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                       RC_CELL_U16, 1000.0f, 0.0f, 1.0f), 1.0f, 1e-4f);
    RC_CHECK_NEAR(rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                       RC_CELL_U16, 1000.0f, 1.0f, 1.0f), 2.0f, 1e-4f);
    RC_CHECK_NEAR(rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                       RC_CELL_U16, 1000.0f, 0.5f, 1.0f), 1.5f, 1e-4f);
    RC_CHECK_NEAR(rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, lumpy, columns,
                                       RC_CELL_U16, 1000.0f, 2.0f, 1.0f), 3.0f, 1e-4f);

    /* A NULL axis is refused outright rather than falling back to an even division: a
     * silent fallback looks exactly like the bug this function exists to fix. */
    RC_CHECK_NEAR(rc_table2d_interp_on(cells, RC_CELL_F32, 3u, 3u, NULL, columns,
                                       RC_CELL_U16, 5000.0f, 0.0f, 1.0f), 0.0f, 1e-6f);

    /* The even-division call still works and still means what it says: a fractional CELL
     * index. Some callers legitimately want that - replaying a table, comparing two offline
     * - and it is the wrong thing to use on a live lookup. */
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_F32, 3u, 3u, 0.0f, 0.0f, 1.0f), 1.0f, 1e-6f);
    /* (1.0, 2.0) is row 1, column 2 of the ramp, which is 6.0. Note the trap this old call
     * invites: 1.0 and 2.0 also read as plausible engineering values, which is how a
     * 12,000 rpm lookup used to end up silently on cell 15. */
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_F32, 3u, 3u, 1.0f, 2.0f, 1.0f), 6.0f, 1e-6f);
    /* Out-of-shape coordinates clamp to the last cell rather than reading past the end. */
    RC_CHECK_NEAR(rc_table2d_interp(cells, RC_CELL_F32, 3u, 3u, 99.0f, 99.0f, 1.0f), 9.0f, 1e-6f);
}



void test_math(void)
{
    RC_TEST_CASE("math/bounds", math_bounds);
    RC_TEST_CASE("axis/locates/the/right/cell", axis_lookup_finds_the_right_cell);
    RC_TEST_CASE("axis/lookup/is/a/search/not/a/validator", axis_lookup_is_a_search_not_a_validator);
    RC_TEST_CASE("a/lumpy/axis/changes/the/answer", a_lumpy_axis_really_does_change_the_answer);
    RC_TEST_CASE("math/angles", math_angles);
    RC_TEST_CASE("math/tables", math_tables);
    RC_TEST_CASE("math/thermistor", math_thermistor);
    RC_TEST_CASE("math/rate/limit", math_rate_limit);
    RC_TEST_CASE("math/decay", math_decay);
    RC_TEST_CASE("math/safe/div", math_safe_div);
    RC_TEST_CASE("crc/vectors", crc_vectors);
    RC_TEST_CASE("crc/digest", crc_digest);
}
