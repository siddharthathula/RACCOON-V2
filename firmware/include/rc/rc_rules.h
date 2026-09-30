/* RACCOON V2 - cross-item rules, mirrored from tools/codegen/rc_rules.py.
 *
 * The build-time rules check the *shipped defaults*; this module checks the *active image*,
 * because a tuner can write any combination and the ECU is the authority
 * (CALIBRATION_SPEC.md section 6).  The rule list itself is GENERATED into
 * firmware/generated/cal_rules.h from the same Python source, so the two tiers cannot
 * disagree about which rules exist.
 */
#ifndef RC_RULES_H
#define RC_RULES_H

#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Severity of a violation, used when a rule is reported to the tuner. */
typedef enum {
    RC_RULE_SEVERITY_INFO = 0,
    RC_RULE_SEVERITY_BLOCKING = 1
} rc_rule_severity_t;

/** One violation, named precisely enough that the tuner can act on it. */
typedef struct {
    uint16_t rule_index;   /**< index into the generated rule name table */
    uint16_t id;           /**< the calibration id involved */
    const char *subject;   /**< the calibration name, or the rule's subject */
    float value;
    rc_rule_severity_t severity;
} rc_rule_fault_t;

/** Number of rules, from the generated table. */
uint16_t rc_rule_count(void);

/** The name of rule `index`, or "invalid" when out of range. */
const char *rc_rule_name(uint16_t index);

#ifdef __cplusplus
}
#endif
#endif /* RC_RULES_H */
