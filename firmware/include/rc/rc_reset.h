/* RACCOON V2 - reset cause. See rc_reset.c. */
#ifndef RC_RESET_H
#define RC_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/rc_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read and validate the latched reset cause. */
uint32_t rc_reset_latch(rc_hal_t *hal);

/** True when the previous session ended abnormally. */
bool rc_reset_was_unexpected(uint32_t reason);

#ifdef __cplusplus
}
#endif
#endif /* RC_RESET_H */
