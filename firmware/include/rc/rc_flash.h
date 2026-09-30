/* RACCOON V2 - calibration storage and the activation transaction.
 *
 * WHY THIS IS SHAPED THE WAY IT IS
 *
 * A motorcycle loses power without warning: the engine is stopped by a stall, a crash, or
 * the rider switching off at a junction. A ECU that writes a calibration in place would
 * therefore sometimes be interrupted halfway through one, and a motorcycle that will not
 * start is a motorcycle on a trailer. Every design decision below exists because of that
 * single fact.
 *
 * The transaction is therefore:
 *
 *   STAGE    write the new image into the INACTIVE slot, with its CRC and a monotonically
 *            increasing version. The running slot is not touched at any point.
 *   VERIFY   read the staged slot back and check it byte for byte against what was
 *            written, and validate it. A slot that cannot be read back is never activated.
 *   ACTIVATE flip one flag, atomically, to select the newly staged slot.
 *   ROLLBACK the previous slot stays valid and selectable for as long as the new one
 *            exists, so a bad tune is one keypress away from being undone.
 *
 * A1: an interruption at ANY point leaves a bootable ECU on the previously active slot.
 *
 * This rests on three things together, not on the write ordering: the image CRC is checked
 * on every read, the header carries its own CRC so a torn header is rejected rather than
 * believed, and every activation re-verifies the slot before flipping the one
 * irreversible flag. See test_flash.c, which sweeps a power cut across the whole
 * transaction and fails if the ECU ever comes up on anything but a complete tune.
 *
 * The activation flag is the only thing written in place, and it is written as a single
 * redundant word pair - never as a value that can be read as "neither slot valid". A
 * partially written flag resolves to the OLD slot, which is the safe direction, because
 * the old tune is the one the engine was demonstrably running on.
 *
 * This module does not know how the flash is actually programmed; that is the HAL's
 * storage_read/storage_write. It owns the transaction, the state machine and the rules.
 */
#ifndef RC_FLASH_H
#define RC_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "cal_types.h"
#include "rc/rc_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Two slots, because a calibration that is only ever written in place cannot be trusted
 *  after a power cut. Sized by the generator: the image is 4396 bytes today and the schema
 *  is allowed to grow, so this must be a bound and not a constant. */
#define RC_FLASH_SLOT_SIZE 8192u
#define RC_FLASH_SLOTS 2u
#define RC_FLASH_MAGIC 0x52414343u /* 'RACC' */

/** Per-slot header. Plain, fixed-width, and explicitly not bit-packed: this is read by
 *  boot code before any initialisation has run, so its layout must not depend on the
 *  compiler's choices. */
typedef struct {
    uint32_t magic;          /**< RC_FLASH_MAGIC once written */
    uint32_t image_crc;      /**< CRC-32 of the calibration image that follows */
    uint32_t version;        /**< monotonic; higher wins, and prevents replaying an old slot */
    uint32_t image_size;     /**< bytes of image stored after this header */
    uint32_t header_crc;     /**< CRC-32 of the preceding 16 bytes, so a torn header is
                              *   itself detectable rather than being believed */
    uint32_t reserved[3];
} rc_flash_header_t;

/** The activation flag: a redundant pair, written one word at a time. */
typedef struct {
    uint32_t active_slot;    /**< which slot the ECU boots from */
    uint32_t active_crc;     /**< CRC of the 4 bytes above */
} rc_flash_marker_t;

typedef enum {
    RC_FLASH_OK = 0,
    RC_FLASH_ERR_PARAM = 1,
    RC_FLASH_ERR_IO = 2,        /**< the medium refused the write */
    RC_FLASH_ERR_CORRUPT = 3,   /**< header or image failed its CRC */
    RC_FLASH_ERR_NO_SLOT = 4,   /**< neither slot holds a valid image */
    RC_FLASH_ERR_BUSY = 5,
    RC_FLASH_ERR_VERSION = 6,   /**< a rollback attempt that would go backwards */
    RC_FLASH_ERR_VALIDATE = 7,  /**< the image is structurally fine but not acceptable */
} rc_flash_result_t;

typedef struct {
    rc_hal_t *hal;
    void *hal_ctx;
    uint32_t base[RC_FLASH_SLOTS];   /**< byte offset of each slot in the medium */
    uint32_t marker_offset;          /**< byte offset of the activation flag */
    uint8_t active;                  /**< cached copy of the active slot */
    uint32_t active_version;
    bool initialised;
} rc_flash_t;

/** The reported state of both slots, for the tuner's diagnostics view. */
typedef struct {
    uint8_t active;
    bool present[RC_FLASH_SLOTS];
    bool verified[RC_FLASH_SLOTS];   /**< read back and CRC-checked this session */
    uint32_t version[RC_FLASH_SLOTS];
    uint32_t image_crc[RC_FLASH_SLOTS];
    uint32_t image_size[RC_FLASH_SLOTS];
    /** True when the active slot is not the newest one written, which means a rollback
     *  has happened and the ECU is running an older tune than the one on the medium. */
    bool rolled_back;
} rc_flash_status_t;

/** Point at the medium and read the activation flag. Must be called before anything else. */
int rc_flash_init(rc_flash_t *flash, rc_hal_t *hal, void *hal_ctx, uint32_t base0,
                  uint32_t base1, uint32_t marker_offset);

/** Read the image the ECU is actually running from. */
int rc_flash_load_active(rc_flash_t *flash, rc_cal_t *out);

/** Read a specific slot. Used by VERIFY and by the tuner's slot browser. */
int rc_flash_read_slot(rc_flash_t *flash, uint8_t slot, rc_cal_t *out, uint32_t *version);

/** STAGE. Writes `cal` into the inactive slot, with its CRC and a higher version. */
int rc_flash_stage(rc_flash_t *flash, const rc_cal_t *cal);

/** VERIFY. Reads the staged slot back and compares it byte for byte with what was
 *  written, then validates it. Nothing is activated on the strength of what was sent. */
int rc_flash_verify_staged(rc_flash_t *flash);

/** ACTIVATE. One flag flip. The previous slot remains valid. */
int rc_flash_activate(rc_flash_t *flash);

/** STAGE + VERIFY + ACTIVATE. Any step failing leaves the ECU on the previous tune. */
int rc_flash_commit(rc_flash_t *flash, const rc_cal_t *cal);

/** Go back to the other slot. Refused if it is invalid. */
int rc_flash_rollback(rc_flash_t *flash);

/** Full status, for the diagnostics view. */
int rc_flash_status(rc_flash_t *flash, rc_flash_status_t *out);

const char *rc_flash_result_text(int result);

#ifdef __cplusplus
}
#endif
#endif /* RC_FLASH_H */
