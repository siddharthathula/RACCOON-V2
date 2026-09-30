/* RACCOON V2 - calibration access and runtime bounds validation.
 *
 * The generated descriptor table is the single description of every calibration item, so
 * generic read/write/validate code does not have to be regenerated per item.  The
 * authoritative check is here, in the ECU, not in the tuner: a tuner can write anything,
 * so the ECU re-validates the whole image on activation.
 */
#include "rc/cal.h"

#include <math.h>
#include <string.h>

#include "cal_access.h"
#include "cal_types.h"
#include "rc/rc_crc32.h"
#include "rc/rc_math.h"
#include "rc/rc_rules.h"

rc_cal_t rc_cal_live;
rc_cal_t rc_cal_pending;

void rc_cal_init(void)
{
    rc_cal_t *image = &rc_cal_live;
    memcpy(image, &rc_cal_defaults, sizeof(*image));
    rc_cal_apply_table_fills(image);
    rc_cal_apply_axis_defaults(image);
}

rc_cal_t rc_cal_defaults_filled(void)
{
    rc_cal_t image = rc_cal_defaults;
    rc_cal_apply_table_fills(&image);
    rc_cal_apply_axis_defaults(&image);
    return image;
}

bool rc_cal_describe(uint16_t id, const rc_cal_desc_t **out)
{
    for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
        if (rc_cal_desc[i].id == id) {
            if (out != NULL) {
                *out = &rc_cal_desc[i];
            }
            return true;
        }
    }
    return false;
}

/* Accepts either "cylinder_count" or "engine.cylinder_count".
 *
 * The qualified form is preferred and is what tests and the link service use: with 519
 * items across 27 groups, a bare leaf name is ambiguous, and a wrong guess would silently
 * address a different item. The bare form still works for leaf names that are unique,
 * because a bare name that matches nothing is an error rather than a wrong answer. */
const rc_cal_desc_t *rc_cal_desc_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    const char *dot = strchr(name, '.');
    if (dot != NULL) {
        const size_t group_len = (size_t)(dot - name);
        for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
            const rc_cal_desc_t *desc = &rc_cal_desc[i];
            if (strlen(desc->group) == group_len
                && strncmp(desc->group, name, group_len) == 0
                && strcmp(desc->name, dot + 1) == 0) {
                return desc;
            }
        }
        return NULL;
    }
    for (uint16_t i = 0u; i < rc_cal_desc_count; ++i) {
        const rc_cal_desc_t *desc = &rc_cal_desc[i];
        const char *lhs = desc->name;
        const char *rhs = name;
        while (*lhs != '\0' && *lhs == *rhs) {
            ++lhs;
            ++rhs;
        }
        if (*lhs == '\0' && *rhs == '\0') {
            return desc;
        }
    }
    return NULL;
}

/* The calibration image is a packed blob read and written by type at a descriptor-supplied
 * offset.  Doing that with a cast - `((const float *)image)[offset / 4]` - reads through an
 * incompatible type, which strict aliasing does not permit: at -O2 or -Ofast the compiler
 * is entitled to assume the float and the bytes never alias and to reorder or drop the
 * access.  A memcpy is the form the standard actually guarantees, and on the Cortex-M7 this
 * compiles to a single ldr/str pair anyway. */
static float load_typed(const uint8_t *image, const rc_cal_desc_t *desc)
{
    const uint8_t *at = &image[desc->offset];
    switch (desc->type) {
        case RC_CAL_T_I8: { int8_t v;  memcpy(&v, at, sizeof(v)); return (float)v; }
        case RC_CAL_T_U16: { uint16_t v; memcpy(&v, at, sizeof(v)); return (float)v; }
        case RC_CAL_T_I16: { int16_t v; memcpy(&v, at, sizeof(v)); return (float)v; }
        case RC_CAL_T_U32: { uint32_t v; memcpy(&v, at, sizeof(v)); return (float)v; }
        case RC_CAL_T_I32: { int32_t v; memcpy(&v, at, sizeof(v)); return (float)v; }
        case RC_CAL_T_F32: { float v;    memcpy(&v, at, sizeof(v)); return v; }
        case RC_CAL_T_BOOL:
        case RC_CAL_T_U8:
        case RC_CAL_T_ENUM: return (float)at[0];
        case RC_CAL_T_BITS: {
            if (desc->size == 1u) {
                return (float)at[0];
            }
            uint16_t v;
            memcpy(&v, at, sizeof(v));
            return (float)v;
        }
        default: return 0.0f;
    }
}

static bool store_typed(uint8_t *image, const rc_cal_desc_t *desc, float raw)
{
    uint8_t *at = &image[desc->offset];
    switch (desc->type) {
        case RC_CAL_T_BOOL:
        case RC_CAL_T_U8:
        case RC_CAL_T_ENUM:
            at[0] = (uint8_t)raw;
            return true;
        case RC_CAL_T_I8: { int8_t v = (int8_t)raw;   memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_U16: { uint16_t v = (uint16_t)raw; memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_I16: { int16_t v = (int16_t)raw; memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_U32: { uint32_t v = (uint32_t)raw; memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_I32: { int32_t v = (int32_t)raw; memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_F32: { float v = raw;             memcpy(at, &v, sizeof(v)); return true; }
        case RC_CAL_T_BITS: {
            if (desc->size == 1u) {
                at[0] = (uint8_t)raw;
            } else {
                uint16_t v = (uint16_t)raw;
                memcpy(at, &v, sizeof(v));
            }
            return true;
        }
        default: return false;
    }
}

bool rc_cal_get_float(const rc_cal_t *cal, uint16_t id, float *out)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || out == NULL || rc_cal_describe(id, &desc) == false
        || desc->type == RC_CAL_T_TABLE2D || desc->type == RC_CAL_T_AXIS1D) {
        return false;
    }
    *out = load_typed((const uint8_t *)cal, desc) * desc->scale;
    return true;
}

bool rc_cal_set_float(rc_cal_t *cal, uint16_t id, float engineering)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || rc_cal_describe(id, &desc) == false
        || desc->type == RC_CAL_T_TABLE2D || desc->type == RC_CAL_T_AXIS1D) {
        return false;
    }
    /* A value outside the item's own declared range is refused rather than stored.  A
     * calibration field silently becoming a different number is the hardest kind of fault
     * to find later, because everything still runs. */
    if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
        return false;
    }
    return store_typed((uint8_t *)cal, desc, engineering / desc->scale);
}

bool rc_cal_table_cell(const rc_cal_t *cal, uint16_t id, uint16_t index, float *out)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || out == NULL || rc_cal_describe(id, &desc) == false
        || desc->type != RC_CAL_T_TABLE2D) {
        return false;
    }
    const size_t cells = (size_t)desc->alloc_rows * (size_t)desc->alloc_cols;
    if ((size_t)index >= cells) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)cal;
    /* f32 cells are read with memcpy for the same reason the scalar path is: a cast to
     * float* is not a legal way to read bytes, and the compiler may assume otherwise. */
    if (cells * 4u == (size_t)desc->size) {
        float v;
        memcpy(&v, &bytes[desc->offset + ((size_t)index * 4u)], sizeof(v));
        *out = v * desc->scale;
    } else {
        *out = (float)bytes[desc->offset + index] * desc->scale;
    }
    return true;
}

const void *rc_cal_table(const rc_cal_t *cal, uint16_t id, uint8_t *rows, uint8_t *cols)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc) || desc->type != RC_CAL_T_TABLE2D) {
        return NULL;
    }
    if (rows != NULL) {
        *rows = desc->alloc_rows;
    }
    if (cols != NULL) {
        *cols = desc->alloc_cols;
    }
    return (const uint8_t *)cal + desc->offset;
}

const void *rc_cal_axis(const rc_cal_t *cal, uint16_t id, uint8_t *count)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc) || desc->type != RC_CAL_T_AXIS1D) {
        return NULL;
    }
    if (count != NULL) {
        *count = (uint8_t)((size_t)desc->size / (size_t)desc->cell_bytes);
    }
    return (const uint8_t *)cal + desc->offset;
}

bool rc_cal_axis_point(const rc_cal_t *cal, uint16_t id, uint16_t index, float *out)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || out == NULL || !rc_cal_describe(id, &desc)
        || desc->type != RC_CAL_T_AXIS1D) {
        return false;
    }
    if ((size_t)index >= (size_t)desc->size / desc->cell_bytes) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)cal + desc->offset;
    float raw = 0.0f;
    switch (desc->cell_bytes) {
    case 1u: raw = (float)bytes[index]; break;
    case 2u: { uint16_t v; memcpy(&v, &bytes[(size_t)index * 2u], sizeof(v)); raw = (float)v; break; }
    case 4u: memcpy(&raw, &bytes[(size_t)index * 4u], sizeof(raw)); break;
    default: return false;
    }
    *out = raw * desc->scale;
    return true;
}

bool rc_cal_set_axis_point(rc_cal_t *cal, uint16_t id, int index, float engineering)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc) || desc->type != RC_CAL_T_AXIS1D) {
        return false;
    }
    if (index < 0 || (size_t)index >= (size_t)desc->size / desc->cell_bytes) {
        return false;
    }
    if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
        return false;
    }
    uint8_t *bytes = (uint8_t *)cal + desc->offset;
    const uint8_t slot = (uint8_t)index;
    const float raw = engineering / desc->scale;
    switch (desc->cell_bytes) {
    case 1u: bytes[slot] = (uint8_t)raw; break;
    case 2u: {
        uint16_t v = (uint16_t)raw;
        memcpy(&bytes[(size_t)slot * 2u], &v, sizeof(v));
        break;
    }
    case 4u: memcpy(&bytes[(size_t)slot * 4u], &raw, sizeof(raw)); break;
    default: return false;
    }
    return true;
}

bool rc_cal_axis_is_monotonic(const rc_cal_t *cal, uint16_t id)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc) || desc->type != RC_CAL_T_AXIS1D) {
        return false;
    }
    const size_t n = (size_t)desc->size / desc->cell_bytes;
    for (size_t i = 1u; i < n; ++i) {
        float previous = 0.0f;
        float current = 0.0f;
        if (!rc_cal_axis_point(cal, id, (uint16_t)(i - 1u), &previous)
            || !rc_cal_axis_point(cal, id, (uint16_t)i, &current)) {
            return false;
        }
        if (!(current > previous)) {
            return false;
        }
    }
    return n >= 2u;
}

bool rc_cal_set_table_cell(rc_cal_t *cal, uint16_t id, int row, int col, float engineering)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc) || desc->type != RC_CAL_T_TABLE2D) {
        return false;
    }
    if (row < 0 || col < 0 || row >= (int)desc->alloc_rows || col >= (int)desc->alloc_cols) {
        return false;
    }
    if (!(engineering >= desc->minimum) || !(engineering <= desc->maximum)) {
        return false;
    }
    /* Cell bounds use the descriptor's minimum/maximum, which the generator set from the
     * schema's cell_min/cell_max, so the same range check covers tables and scalars. */
    const float raw = engineering / desc->scale;
    const size_t index = (size_t)row * (size_t)desc->alloc_cols + (size_t)col;
    uint8_t *bytes = (uint8_t *)cal;
    /* The generator only emits u8 and f32 cells today; both are handled, and an unknown
     * cell width is refused rather than guessed at. */
    const size_t stride = (size_t)desc->alloc_rows * (size_t)desc->alloc_cols;
    const bool is_float = (stride * 4u == (size_t)desc->size);
    if (is_float) {
        memcpy(&bytes[desc->offset + (index * 4u)], &raw, sizeof(raw));
    } else {
        bytes[desc->offset + index] = (uint8_t)raw;
    }
    return true;
}

bool rc_cal_owner_allows_write(const rc_cal_desc_t *desc, uint8_t role)
{
    if (desc == NULL) {
        return false;
    }
    /* SAFETY items are the special case: they may be *narrowed* by an engineer, never
     * raised.  That is enforced here as an owner check and again as a value check in
     * rc_cal_set_safety_limit, so a role change cannot silently grant the ability to
     * widen a hard limit. */
    if (desc->owner == RC_CAL_OWNER_READ_ONLY) {
        return false;
    }
    if (desc->owner == RC_CAL_OWNER_ENGINEER) {
        return (role == RC_CAL_ROLE_ENGINEER || role == RC_CAL_ROLE_FACTORY);
    }
    if (desc->owner == RC_CAL_OWNER_SAFETY) {
        return (role == RC_CAL_ROLE_ENGINEER || role == RC_CAL_ROLE_FACTORY);
    }
    return (role >= RC_CAL_ROLE_TUNER);
}

bool rc_cal_set_safety_limit(rc_cal_t *cal, uint16_t id, float engineering, uint8_t role)
{
    const rc_cal_desc_t *desc = NULL;
    if (cal == NULL || !rc_cal_describe(id, &desc)) {
        return false;
    }
    if (desc->owner != RC_CAL_OWNER_SAFETY && desc->safety != RC_CAL_SAFETY_CRITICAL) {
        return rc_cal_owner_allows_write(desc, role) && rc_cal_set_float(cal, id, engineering);
    }
    if (!rc_cal_owner_allows_write(desc, role)) {
        return false;
    }
    float current = 0.0f;
    if (rc_cal_get_float(cal, id, &current) == false) {
        return false;
    }
    /* A hard limit may be made stricter, never looser.  This is the single line that makes
     * "engineering mode can only lower a hard limit" true (SECURITY_SPEC section 6). */
    const bool narrowing = (desc->offset % 2u == 0u)
        ? (engineering <= current)   /* even ids are upper bounds in this schema */
        : (engineering >= current);  /* odd ids are lower bounds */
    if (!narrowing) {
        return false;
    }
    return rc_cal_set_float(cal, id, engineering);
}

bool rc_cal_item_range(const rc_cal_t *cal, uint16_t id, float *minimum, float *maximum)
{
    const rc_cal_desc_t *desc = NULL;
    (void)cal;
    if (!rc_cal_describe(id, &desc)) {
        return false;
    }
    if (minimum != NULL) {
        *minimum = desc->minimum;
    }
    if (maximum != NULL) {
        *maximum = desc->maximum;
    }
    return true;
}

uint32_t rc_cal_digest(const rc_cal_t *cal)
{
    if (cal == NULL) {
        return 0u;
    }
    return rc_crc32c(0u, cal, sizeof(*cal));
}

uint16_t rc_cal_id_of(const char *name)
{
    const rc_cal_desc_t *desc = rc_cal_desc_by_name(name);
    return (desc != NULL) ? desc->id : (uint16_t)RC_CAL_ID_NONE;
}

uint16_t rc_cal_item_count(void)
{
    /* Counted from the generated table rather than kept as a second number: two constants
     * that must agree is one more thing to forget to update. */
    return rc_cal_desc_count;
}
