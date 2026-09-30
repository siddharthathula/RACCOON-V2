/* RACCOON V2 - reset cause and safe-state bookkeeping.
 *
 * Power loss is a normal operating condition for a motorcycle ECU, not an exception
 * (spec section 105).  The reset cause is latched before the CPU is allowed to do
 * anything else, and the flag that says "the engine was running" lives in a backup
 * region that survives a brownout, so an unexpected reset is distinguishable from a
 * deliberate power cycle.
 */
#include "rc/rc_reset.h"

#include "rc/rc_hal.h"

const char *rc_reset_reason_name(uint32_t reason)
{
    switch (reason) {
        case RC_RESET_POWER_ON: return "power_on";
        case RC_RESET_PIN: return "pin";
        case RC_RESET_SOFTWARE: return "software";
        case RC_RESET_WATCHDOG: return "watchdog";
        case RC_RESET_BROWNOUT: return "brownout";
        case RC_RESET_HARD_FAULT: return "hard_fault";
        case RC_RESET_UNKNOWN:
        default: return "unknown";
    }
}

uint32_t rc_reset_latch(rc_hal_t *hal)
{
    uint32_t reason = RC_RESET_UNKNOWN;
    if (hal != NULL && hal->reset_reason != NULL) {
        hal->reset_reason((void *)hal, &reason);
    }
    return reason;
}

bool rc_reset_was_unexpected(uint32_t reason)
{
    /* A watchdog reset or a brownout while running means the previous session ended
     * abnormally.  A plain power-on does not: every power cycle is a reset. */
    return reason == RC_RESET_WATCHDOG || reason == RC_RESET_BROWNOUT
        || reason == RC_RESET_HARD_FAULT;
}
