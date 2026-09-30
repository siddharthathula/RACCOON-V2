/* RACCOON V2 - portable numeric helpers.
 *
 * Everything here is O(1), allocation-free and safe to call from the real-time region.
 * INDEPENDENTLY AUTHORED.
 */
#ifndef RC_MATH_H
#define RC_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RC_PI_F
#define RC_PI_F 3.14159265358979323846f
#endif

/** Clamp to a range. NaN falls to `lo`, so a poisoned input can never escape upward. */
static inline float rc_clampf(float value, float lo, float hi)
{
    if (!(value == value)) { /* NaN */
        return lo;
    }
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static inline int32_t rc_clampi32(int32_t value, int32_t lo, int32_t hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static inline float rc_absf(float value)
{
    return (value < 0.0f) ? -value : value;
}

static inline float rc_maxf(float a, float b)
{
    return (a > b) ? a : b;
}

static inline float rc_minf(float a, float b)
{
    return (a < b) ? a : b;
}

/** Sign with a deadband: returns 0 inside +/- deadband. */
static inline float rc_deadband(float value, float deadband)
{
    if (value > deadband) {
        return value - deadband;
    }
    if (value < -deadband) {
        return value + deadband;
    }
    return 0.0f;
}

/** Wrap to [0, modulus). Correct for negative inputs. */
static inline float rc_wrapf(float value, float modulus)
{
    if (modulus <= 0.0f) {
        return 0.0f;
    }
    float wrapped = value;
    while (wrapped < 0.0f) {
        wrapped += modulus;
    }
    while (wrapped >= modulus) {
        wrapped -= modulus;
    }
    return wrapped;
}

/** Shortest signed difference between two angles, in (-modulus/2, modulus/2]. */
float rc_angle_delta(float from_deg, float to_deg, float modulus);

/** Storage kind of a table's cells.
 *
 * Explicit, because a table of u8 cells read through a float pointer is a silent and
 * catastrophic memory misinterpretation: the calibration image is mostly byte cells.
 */
typedef enum {
    RC_CELL_U8 = 0,
    RC_CELL_U16 = 1,
    RC_CELL_F32 = 2
} rc_cell_kind_t;

/** Bilinear interpolation on a rectangular table.
 *
 * @param table       row-major, `cols` wide
 * @param rows,cols   active shape
 * @param row         fractional row coordinate, clamped to [0, rows-1]
 * @param col         fractional column coordinate, clamped to [0, cols-1]
 * @param cells       the raw cell array
 * @param kind        storage kind of the cells
 * @param scale       engineering units per raw cell unit
 *
 * Clamping rather than extrapolating is deliberate: an out-of-range cell coordinate means
 * the axis and the value disagree, and silently extrapolating a fuel or spark command
 * from there is how a table edge becomes an engine-destroying number.
 */
float rc_table2d_interp(const void *cells, rc_cell_kind_t kind, uint8_t rows, uint8_t cols,
                        float row, float col, float scale);

/** Locate a value on a monotonic axis: the two breakpoints either side of it, and how far
 *  between them it sits.
 *
 *  The axis MUST be monotonic. This is a search, not a validator: a binary search over an
 *  unsorted array can land on a perfectly well-formed pair from the part that happens to be
 *  in order, and return a plausible answer. The guarantee that an axis is monotonic comes
 *  from rc_cal_axis_is_monotonic(), which examines every pair and runs at activation.
 *  This function refuses only a bracket IT lands on that is degenerate.
 *
 *  This is the function that makes an axis worth storing. Without it a table is indexed by
 *  an even division of its range, so the low end - where an engine spends most of its life
 *  and where resolution is worth the most - gets exactly as much as the top end that is
 *  never visited. With it, the tuner moves the breakpoints and the resolution follows.
 *
 *  @param axis   raw breakpoints, stored ascending
 *  @param kind   storage kind of the breakpoints
 *  @param count  number of breakpoints, 2 or more
 *  @param value  the coordinate being located
 *  @param lo     index of the breakpoint at or below `value` (clamped to 0)
 *  @param hi     index of the next breakpoint, equal to `lo` at the ends
 *  @param frac   0..1 between `lo` and `hi`; 0 outside the axis, no extrapolation
 *
 *  A bracket that is not strictly increasing is refused by returning false rather than
 *  divided by, so a degenerate pair cannot produce a number here at all.
 */
bool rc_axis_locate(const void *axis, rc_cell_kind_t kind, uint8_t count, float value,
                    uint8_t *lo, uint8_t *hi, float *frac);

/** Bilinear interpolation along two monotonic axes.
 *
 *  Same as rc_table2d_interp(), except `row` and `col` are ENGINEERING coordinates looked
 *  up on `row_axis` and `col_axis` rather than fractional cell indices. Values outside an
 *  axis are clamped to its end cell - never extrapolated, because extrapolating past the
 *  last breakpoint a tuner set is the ECU inventing data.
 *
 *  @param cell_kind   storage kind of the CELLS
 *  @param axis_kind   storage kind of the BREAKPOINTS - a separate argument because the two
 *                     are genuinely different widths in this schema: a VE table stores u8
 *                     cells, because a VE of 72.5 percent needs no more than a byte, and
 *                     u16 breakpoints, because rpm runs to 12,000. One shared `kind` would
 *                     read the axis as bytes and turn 12,000 rpm into 4,656.
 *  @param scale       cell scale, as for rc_table2d_interp()
 */
float rc_table2d_interp_on(const void *cells, rc_cell_kind_t cell_kind, uint8_t rows,
                           uint8_t cols, const void *row_axis, const void *col_axis,
                           rc_cell_kind_t axis_kind, float row, float col, float scale);

/** Read one cell in engineering units, with the same clamping. */
float rc_table2d_cell(const void *cells, rc_cell_kind_t kind, uint8_t rows, uint8_t cols,
                      int row, int col, float scale);

/** Steinhart-Hart, full B equation. R in ohms, T in kelvin. */
float rc_steinhart_hart(float resistance_ohm, float b_coeff, float ref_resistance_ohm, float ref_kelvin);

/** Resistance to kelvin. */
float rc_r_to_kelvin(float resistance_ohm, float b_coeff, float ref_resistance_ohm, float ref_kelvin);

/** First-order low-pass step. `alpha` in 0..1; 1 disables the filter. */
float rc_lowpass(float previous, float target, float alpha);

/**
 * Rate limiter.
 *
 * @return `value` moved towards `target` by at most `max_delta`.
 *
 * Distinct from rc_lowpass: a rate limit is a *hard* bound on how fast a value may
 * change, which is how a physical impossibility (a throttle cannot open 200 % in a
 * millisecond) is enforced, and the limiter stays visible through its own flag.
 */
float rc_rate_limit(float value, float target, float max_delta, bool *was_limited);

/** First-order transient term: an exponential decay with a time constant. */
float rc_decay(float state, float target, float dt_s, float tau_s);

/** Idle-safe division: returns `fallback` when the denominator is too small. */
float rc_safe_div(float numerator, float denominator, float fallback);

#ifdef __cplusplus
}
#endif
#endif /* RC_MATH_H */
