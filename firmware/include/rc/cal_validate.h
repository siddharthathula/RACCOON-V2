/* RACCOON V2 - runtime calibration validation. */
#ifndef RC_CAL_VALIDATE_H
#define RC_CAL_VALIDATE_H

#include "cal_types.h"
#include "rc/rc_rules.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RC_SEVERITY_INFO = 0,
    RC_SEVERITY_WARNING = 1,
    RC_SEVERITY_BLOCKING = 2
} rc_severity_t;

/** One violation.  `rule_index` is RC_RULE_INDEX_NONE for a per-item bound violation,
 *  otherwise it indexes the generated rule name table. */
#define RC_RULE_INDEX_NONE 0xFFFFu

typedef struct {
    uint16_t rule_index;
    uint16_t id;            /**< the calibration id */
    const char *name;       /**< the calibration name, or the rule's subject */
    float value;
    rc_severity_t severity;
} rc_cal_fault_t;

/** Per-item bounds: every value inside its declared range, every table cell too. */
uint32_t rc_cal_validate_items(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity);

/** Cross-item rules: the runtime mirror of tools/codegen/rc_rules.py. */
uint32_t rc_cal_validate_rules(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity);

/**
 * Full validation.
 *
 * @return true when the image is armable; otherwise `blocking_count` says how many
 *         violations were found, and `faults` holds up to `capacity` of them.
 */
bool rc_cal_validate(const rc_cal_t *cal, rc_cal_fault_t *faults, uint32_t capacity,
                     uint32_t *blocking_count);

/** Prose for a rule, shown in the tuner's refusal dialog. */
const char *rc_rule_text(uint16_t rule_index);

const char *rc_cal_severity_name(rc_severity_t severity);

#ifdef __cplusplus
}
#endif
#endif /* RC_CAL_VALIDATE_H */
