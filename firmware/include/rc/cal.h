/* RACCOON V2 - calibration runtime API. */
#ifndef RC_CAL_H
#define RC_CAL_H

#include "cal_types.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Session roles, in ascending privilege. Mirrors rc_cal_owner_t's counterpart. */
#define RC_CAL_ROLE_VIEWER 0u
#define RC_CAL_ROLE_TUNER 1u
#define RC_CAL_ROLE_ENGINEER 2u
#define RC_CAL_ROLE_FACTORY 3u

/** The live image the control loop reads, and the staging image a write is built in. */
extern rc_cal_t rc_cal_live;
extern rc_cal_t rc_cal_pending;

/** Copy the shipped defaults into the live image, including table fills and axes. */
void rc_cal_init(void);

/** Initialise every table to its declared fill value. Generated. */
void rc_cal_apply_table_fills(rc_cal_t *image);

/** Lay every axis out evenly across its declared range. Generated.  An evenly spaced
 * axis is the correct STARTING point, not a claim about the engine: it puts resolution
 * everywhere at once, which is worse than putting it where the engine actually lives. */
void rc_cal_apply_axis_defaults(rc_cal_t *image);

/** The shipped defaults with the table fills applied - what the ECU actually starts from. */
rc_cal_t rc_cal_defaults_filled(void);

/** Look up a descriptor by id. */
bool rc_cal_describe(uint16_t id, const rc_cal_desc_t **out);

/** Look up a descriptor by name, without allocating. */
const rc_cal_desc_t *rc_cal_desc_by_name(const char *name);

/** Id for a "group.name" string, or 0xFFFF when there is no such item.
 *
 *  Exists so callers and tests name calibration items instead of numbering them. A test
 *  that says `rc_cal_id_of("engine.cylinder_count")` says what it means; the same test
 *  saying `0x0107` says nothing, and silently tests a different item if the schema is ever
 *  renumbered. */
#define RC_CAL_ID_NONE 0xFFFFu
uint16_t rc_cal_id_of(const char *name);

/** How many calibration items exist. The same count the descriptor table reports. */
uint16_t rc_cal_item_count(void);

/** Read a scalar in engineering units. */
bool rc_cal_get_float(const rc_cal_t *cal, uint16_t id, float *out);

/** Write a scalar in engineering units; refuses a value outside the schema bounds. */
bool rc_cal_set_float(rc_cal_t *cal, uint16_t id, float engineering);

/** Pointer to a table's cells, with its allocated shape. */
const void *rc_cal_table(const rc_cal_t *cal, uint16_t id, uint8_t *rows, uint8_t *cols);

/** Raw storage of an axis1d item, and its breakpoint count.  NULL if `id` is not an axis.
 *  `count` is the number of BREAKPOINTS, which is not the same number as `size` in the
 *  descriptor (that one counts storage units), and a host that confuses the two reads one
 *  breakpoint too many - off the end of the array, and into the next item. */
const void *rc_cal_axis(const rc_cal_t *cal, uint16_t id, uint8_t *count);

/** One breakpoint of an axis in engineering units. */
bool rc_cal_axis_point(const rc_cal_t *cal, uint16_t id, uint16_t index, float *out);

/** Write one breakpoint.  Range-checked against the axis's own declared range, but NOT
 *  against its neighbours: ordering is a property of the whole axis, and it is checked
 *  when the axis is validated.  A per-point neighbour check would make it impossible to
 *  drag a breakpoint past its neighbour - which is exactly the edit a tuner makes. */
bool rc_cal_set_axis_point(rc_cal_t *cal, uint16_t id, int index, float engineering);

/** True when every breakpoint is strictly greater than the one before it.  A repeated or
 *  inverted pair makes the axis non-invertible, and a bilinear search over it either
 *  loops or silently returns the wrong cell. */
bool rc_cal_axis_is_monotonic(const rc_cal_t *cal, uint16_t id);

/** One table cell in engineering units, by linear index. False if the id is not a table
 * or the index is out of range. Used by the validator, which must not type-pun the image. */
bool rc_cal_table_cell(const rc_cal_t *cal, uint16_t id, uint16_t index, float *out);

/** Write one table cell in engineering units; refuses out-of-range or out-of-shape. */
bool rc_cal_set_table_cell(rc_cal_t *cal, uint16_t id, int row, int col, float engineering);

/** True when `role` may write an item of this owner class. */
bool rc_cal_owner_allows_write(const rc_cal_desc_t *desc, uint8_t role);

/**
 * Write a safety-critical item, which may only ever be made *stricter*.
 *
 * The even/odd parity of the calibration id is the declared direction: an even id is an
 * upper bound and an odd id is a lower bound.  That convention is asserted in
 * `rc_rules.py` and in `test_calibration.c`, so it cannot be quietly violated.
 */
bool rc_cal_set_safety_limit(rc_cal_t *cal, uint16_t id, float engineering, uint8_t role);

/** Declared engineering range of any item, including a table's cell range. */
bool rc_cal_item_range(const rc_cal_t *cal, uint16_t id, float *minimum, float *maximum);

/** CRC-32C over the whole image; the value that a commit record carries. */
uint32_t rc_cal_digest(const rc_cal_t *cal);

#ifdef __cplusplus
}
#endif
#endif /* RC_CAL_H */
