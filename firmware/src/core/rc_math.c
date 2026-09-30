/* RACCOON V2 - portable numeric helpers. See rc_math.h. */
#include "rc/rc_math.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static float raw_cell(const void *cells, rc_cell_kind_t kind, size_t index)
{
    switch (kind) {
        case RC_CELL_U8:
            return (float)((const uint8_t *)cells)[index];
        case RC_CELL_U16:
            return (float)((const uint16_t *)cells)[index];
        case RC_CELL_F32:
        default:
            return ((const float *)cells)[index];
    }
}

float rc_angle_delta(float from_deg, float to_deg, float modulus)
{
    if (!(modulus > 0.0f)) {
        return 0.0f;
    }
    float delta = rc_wrapf(to_deg - from_deg, modulus);
    const float half = modulus * 0.5f;
    if (delta > half) {
        delta -= modulus;
    }
    return delta;
}

float rc_table2d_cell(const void *cells, rc_cell_kind_t kind, uint8_t rows, uint8_t cols,
                      int row, int col, float scale)
{
    if (cells == NULL || rows == 0u || cols == 0u) {
        return 0.0f;
    }
    /* Clamp on both axes.  A row or column outside the active shape is a disagreement
     * between the axis definition and the value; refusing to extrapolate is deliberate. */
    if (row < 0) {
        row = 0;
    }
    if (col < 0) {
        col = 0;
    }
    if (row >= (int)rows) {
        row = (int)rows - 1;
    }
    if (col >= (int)cols) {
        col = (int)cols - 1;
    }
    return raw_cell(cells, kind, (size_t)row * (size_t)cols + (size_t)col) * scale;
}

static float rc_axis_at(const void *axis, rc_cell_kind_t kind, uint8_t index)
{
    switch (kind) {
    case RC_CELL_U16: return (float)((const uint16_t *)axis)[index];
    case RC_CELL_F32: return ((const float *)axis)[index];
    case RC_CELL_U8:
    default: return (float)((const uint8_t *)axis)[index];
    }
}

bool rc_axis_locate(const void *axis, rc_cell_kind_t kind, uint8_t count, float value,
                    uint8_t *lo, uint8_t *hi, float *frac)
{
    if (axis == NULL || lo == NULL || hi == NULL || frac == NULL || count < 2u) {
        return false;
    }
    if (!(value == value)) {
        return false; /* NaN has no position on any axis */
    }
    const float first = rc_axis_at(axis, kind, 0u);
    const float last = rc_axis_at(axis, kind, (uint8_t)(count - 1u));
    /* Below or above the axis: clamp to the end cell and take zero span. The alternative -
     * extrapolating - has the ECU inventing fuel and spark beyond what anyone measured. */
    if (value <= first) {
        *lo = 0u;
        *hi = 0u;
        *frac = 0.0f;
        return true;
    }
    if (value >= last) {
        *lo = (uint8_t)(count - 1u);
        *hi = *lo;
        *frac = 0.0f;
        return true;
    }
    /* Binary search for the last breakpoint at or below `value`. A calibrated axis is 8 to
     * 16 long, so a linear scan would also be fine; the binary form is here because the
     * cost of a wrong answer is not proportional to the cost of being slow. */
    uint8_t low = 0u;
    uint8_t high = (uint8_t)(count - 1u);
    while ((uint8_t)(high - low) > 1u) {
        const uint8_t mid = (uint8_t)(low + ((high - low) / 2u));
        if (rc_axis_at(axis, kind, mid) <= value) {
            low = mid;
        } else {
            high = mid;
        }
    }
    const float a = rc_axis_at(axis, kind, low);
    const float b = rc_axis_at(axis, kind, high);
    *lo = low;
    *hi = high;
    if (!(b > a)) {
        /* An inverted or repeated pair. Refuse rather than divide by zero or divide by a
         * negative number, both of which would return a plausible value from a table whose
         * axis the ECU does not understand. */
        return false;
    }
    *frac = (value - a) / (b - a);
    return true;
}

float rc_table2d_interp_on(const void *cells, rc_cell_kind_t cell_kind, uint8_t rows,
                           uint8_t cols, const void *row_axis, const void *col_axis,
                           rc_cell_kind_t axis_kind, float row, float col, float scale)
{
    if (cells == NULL || row_axis == NULL || col_axis == NULL || rows == 0u || cols == 0u) {
        return 0.0f;
    }
    uint8_t r0 = 0u;
    uint8_t r1 = 0u;
    uint8_t c0 = 0u;
    uint8_t c1 = 0u;
    float fr = 0.0f;
    float fc = 0.0f;
    if (rows > 1u) {
        if (!rc_axis_locate(row_axis, axis_kind, rows, row, &r0, &r1, &fr)) {
            /* An axis the ECU cannot search: fall back to the low corner rather than to a
             * number derived from an axis it does not understand. */
            r0 = 0u;
            r1 = 0u;
            fr = 0.0f;
        }
    }
    if (cols > 1u) {
        if (!rc_axis_locate(col_axis, axis_kind, cols, col, &c0, &c1, &fc)) {
            c0 = 0u;
            c1 = 0u;
            fc = 0.0f;
        }
    }
    const float v00 = rc_table2d_cell(cells, cell_kind, rows, cols, (int)r0, (int)c0, scale);
    const float v01 = rc_table2d_cell(cells, cell_kind, rows, cols, (int)r0, (int)c1, scale);
    const float v10 = rc_table2d_cell(cells, cell_kind, rows, cols, (int)r1, (int)c0, scale);
    const float v11 = rc_table2d_cell(cells, cell_kind, rows, cols, (int)r1, (int)c1, scale);
    const float top = v00 + (v01 - v00) * fc;
    const float bottom = v10 + (v11 - v10) * fc;
    return top + (bottom - top) * fr;
}

float rc_table2d_interp(const void *cells, rc_cell_kind_t kind, uint8_t rows, uint8_t cols,
                        float row, float col, float scale)
{
    if (cells == NULL || rows == 0u || cols == 0u) {
        return 0.0f;
    }
    if (rows == 1u && cols == 1u) {
        return rc_table2d_cell(cells, kind, rows, cols, 0, 0, scale);
    }
    if (!(row == row) || !(col == col)) {
        return rc_table2d_cell(cells, kind, rows, cols, 0, 0, scale);
    }

    const float max_row = (float)(rows - 1u);
    const float max_col = (float)(cols - 1u);
    if (row < 0.0f) {
        row = 0.0f;
    }
    if (col < 0.0f) {
        col = 0.0f;
    }
    if (row > max_row) {
        row = max_row;
    }
    if (col > max_col) {
        col = max_col;
    }

    const uint8_t r0 = (uint8_t)row;
    const uint8_t c0 = (uint8_t)col;
    /* At the last row/column the "next" cell is the same cell, which is exactly the
     * correct clamped behaviour and avoids a special case at the edges. */
    const uint8_t r1 = (uint8_t)((row >= max_row) ? r0 : (r0 + 1u));
    const uint8_t c1 = (uint8_t)((col >= max_col) ? c0 : (c0 + 1u));
    const float fr = row - (float)r0;
    const float fc = col - (float)c0;

    const float v00 = rc_table2d_cell(cells, kind, rows, cols, (int)r0, (int)c0, scale);
    const float v01 = rc_table2d_cell(cells, kind, rows, cols, (int)r0, (int)c1, scale);
    const float v10 = rc_table2d_cell(cells, kind, rows, cols, (int)r1, (int)c0, scale);
    const float v11 = rc_table2d_cell(cells, kind, rows, cols, (int)r1, (int)c1, scale);

    const float top = v00 + (v01 - v00) * fc;
    const float bottom = v10 + (v11 - v10) * fc;
    return top + (bottom - top) * fr;
}

float rc_r_to_kelvin(float resistance_ohm, float b_coeff, float ref_resistance_ohm, float ref_kelvin)
{
    if (!(resistance_ohm > 0.0f) || !(ref_resistance_ohm > 0.0f) || !(b_coeff > 0.0f)) {
        return 0.0f;
    }
    const float inv_t = (1.0f / ref_kelvin) + (1.0f / b_coeff) * logf(resistance_ohm / ref_resistance_ohm);
    if (!(inv_t > 0.0f)) {
        return 0.0f;
    }
    return 1.0f / inv_t;
}

float rc_steinhart_hart(float resistance_ohm, float b_coeff, float ref_resistance_ohm, float ref_kelvin)
{
    const float kelvin = rc_r_to_kelvin(resistance_ohm, b_coeff, ref_resistance_ohm, ref_kelvin);
    return kelvin - 273.15f;
}

float rc_lowpass(float previous, float target, float alpha)
{
    if (alpha >= 1.0f) {
        return target;
    }
    if (alpha <= 0.0f) {
        return previous;
    }
    return previous + (target - previous) * alpha;
}

float rc_rate_limit(float value, float target, float max_delta, bool *was_limited)
{
    const float delta = target - value;
    if (rc_absf(delta) <= max_delta) {
        if (was_limited != NULL) {
            *was_limited = false;
        }
        return target;
    }
    if (was_limited != NULL) {
        *was_limited = true;
    }
    return value + ((delta > 0.0f) ? max_delta : -max_delta);
}

float rc_decay(float state, float target, float dt_s, float tau_s)
{
    if (!(tau_s > 0.0f)) {
        return target;
    }
    if (dt_s <= 0.0f) {
        return state;
    }
    const float alpha = 1.0f - expf(-dt_s / tau_s);
    return state + (target - state) * alpha;
}

float rc_safe_div(float numerator, float denominator, float fallback)
{
    const float magnitude = rc_absf(denominator);
    if (!(magnitude > 1.0e-9f)) {
        return fallback;
    }
    return numerator / denominator;
}
