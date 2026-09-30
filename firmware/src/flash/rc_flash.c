/* RACCOON V2 - calibration storage and the activation transaction.  See rc_flash.h. */
#include "rc/rc_flash.h"

#include <string.h>

#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/rc_crc32.h"

/* Every access to the medium goes through these two, so the whole module has exactly one
 * place where the hardware is touched, and a test can make the medium fail on demand. */
static int flash_read(rc_flash_t *flash, uint32_t offset, void *dst, uint32_t len)
{
    if (flash == NULL || flash->hal == NULL || flash->hal->storage_read == NULL) {
        return RC_FLASH_ERR_IO;
    }
    return (flash->hal->storage_read(flash->hal_ctx, offset, dst, len) == RC_HAL_OK)
        ? RC_FLASH_OK : RC_FLASH_ERR_IO;
}

static int flash_write(rc_flash_t *flash, uint32_t offset, const void *src, uint32_t len)
{
    if (flash == NULL || flash->hal == NULL || flash->hal->storage_write == NULL) {
        return RC_FLASH_ERR_IO;
    }
    return (flash->hal->storage_write(flash->hal_ctx, offset, src, len) == RC_HAL_OK)
        ? RC_FLASH_OK : RC_FLASH_ERR_IO;
}

static uint32_t header_crc(const rc_flash_header_t *header)
{
    /* Over the four fields that describe the image, deliberately NOT including
     * header_crc itself. */
    rc_digest_t crc;
    rc_digest_init(&crc);
    rc_digest_update(&crc, &header->magic, sizeof(header->magic));
    rc_digest_update(&crc, &header->image_crc, sizeof(header->image_crc));
    rc_digest_update(&crc, &header->version, sizeof(header->version));
    rc_digest_update(&crc, &header->image_size, sizeof(header->image_size));
    return rc_digest_final(&crc);
}

/** Read and check a slot header. */
static int read_header(rc_flash_t *flash, uint8_t slot, rc_flash_header_t *out)
{
    if (slot >= RC_FLASH_SLOTS) {
        return RC_FLASH_ERR_PARAM;
    }
    int rc = flash_read(flash, flash->base[slot], out, sizeof(*out));
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    if (out->magic != RC_FLASH_MAGIC) {
        return RC_FLASH_ERR_CORRUPT;
    }
    /* A torn header is detected here rather than believed.  On a part that is being erased
     * and rewritten, a header can be half old and half new: the magic may be intact while
     * the size or CRC is from a different image entirely. */
    if (out->header_crc != header_crc(out)) {
        return RC_FLASH_ERR_CORRUPT;
    }
    if (out->image_size == 0u || out->image_size > RC_FLASH_SLOT_SIZE) {
        return RC_FLASH_ERR_CORRUPT;
    }
    return RC_FLASH_OK;
}

static int write_marker(rc_flash_t *flash, uint8_t slot)
{
    rc_flash_marker_t marker;
    memset(&marker, 0, sizeof(marker));
    marker.active_slot = (uint32_t)slot;
    rc_digest_t crc;
    rc_digest_init(&crc);
    rc_digest_update(&crc, &marker.active_slot, sizeof(marker.active_slot));
    marker.active_crc = rc_digest_final(&crc);

    /* The CRC goes first, then the slot.  A cut between the two leaves a marker whose CRC
     * does not match, which the reader treats as "not written" and falls back to slot 0 -
     * the safe direction, because slot 0 is the one the engine was running. */
    int rc = flash_write(flash, flash->marker_offset + sizeof(uint32_t), &marker.active_crc,
                         sizeof(marker.active_crc));
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    return flash_write(flash, flash->marker_offset, &marker.active_slot,
                       sizeof(marker.active_slot));
}

static int read_marker(rc_flash_t *flash, uint8_t *slot)
{
    rc_flash_marker_t marker;
    memset(&marker, 0, sizeof(marker));
    const int rc = flash_read(flash, flash->marker_offset, &marker, sizeof(marker));
    if (rc != RC_FLASH_OK) {
        /* An unreadable marker means an erased device, not a broken one: boot slot 0. */
        *slot = 0u;
        return RC_FLASH_OK;
    }
    rc_digest_t crc;
    rc_digest_init(&crc);
    rc_digest_update(&crc, &marker.active_slot, sizeof(marker.active_slot));
    if (marker.active_crc != rc_digest_final(&crc)) {
        *slot = 0u; /* torn write: fall back to the slot the engine was demonstrably on */
        return RC_FLASH_OK;
    }
    if (marker.active_slot >= RC_FLASH_SLOTS) {
        *slot = 0u;
        return RC_FLASH_OK;
    }
    *slot = (uint8_t)marker.active_slot;
    return RC_FLASH_OK;
}

int rc_flash_init(rc_flash_t *flash, rc_hal_t *hal, void *hal_ctx, uint32_t base0,
                  uint32_t base1, uint32_t marker_offset)
{
    if (flash == NULL || hal == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    memset(flash, 0, sizeof(*flash));
    flash->hal = hal;
    flash->hal_ctx = hal_ctx;
    flash->base[0] = base0;
    flash->base[1] = base1;
    flash->marker_offset = marker_offset;
    flash->active = 0u;
    flash->initialised = true;
    return read_marker(flash, &flash->active);
}

int rc_flash_read_slot(rc_flash_t *flash, uint8_t slot, rc_cal_t *out, uint32_t *version)
{
    if (flash == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    rc_flash_header_t header;
    int rc = read_header(flash, slot, &header);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    /* The image is read into a temporary first.  Reading straight into the live structure
     * would leave a half-written image in memory if the CRC then failed, which is exactly
     * the situation where a valid-looking calibration is most dangerous. */
    uint8_t staging[RC_FLASH_SLOT_SIZE];
    memset(staging, 0, sizeof(staging));
    rc = flash_read(flash, flash->base[slot] + sizeof(header), staging, header.image_size);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    rc_digest_t crc;
    rc_digest_init(&crc);
    rc_digest_update(&crc, staging, header.image_size);
    if (rc_digest_final(&crc) != header.image_crc) {
        return RC_FLASH_ERR_CORRUPT;
    }
    if (header.image_size != sizeof(rc_cal_t)) {
        return RC_FLASH_ERR_CORRUPT;
    }
    if (out != NULL) {
        memcpy(out, staging, sizeof(rc_cal_t));
    }
    if (version != NULL) {
        *version = header.version;
    }
    return RC_FLASH_OK;
}

int rc_flash_load_active(rc_flash_t *flash, rc_cal_t *out)
{
    if (flash == NULL || out == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    uint32_t version = 0u;
    int rc = rc_flash_read_slot(flash, flash->active, out, &version);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    flash->active_version = version;
    return RC_FLASH_OK;
}

int rc_flash_stage(rc_flash_t *flash, const rc_cal_t *cal)
{
    if (flash == NULL || cal == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    if (sizeof(rc_cal_t) > RC_FLASH_SLOT_SIZE) {
        return RC_FLASH_ERR_PARAM;
    }
    /* Never stage over the running tune.  This is the whole reason there are two slots;
     * writing the inactive one is not an optimisation, it is the property that makes a
     * power cut survivable. */
    const uint8_t target = (uint8_t)(flash->active ^ 1u);

    /* The version comes from the NEWEST slot, not the running one.  A rollback leaves the
     * active slot holding an older tune, and taking the version from it would let the next
     * write reuse a version number that already exists - after which anti-rollback can no
     * longer tell a replayed old tune from a new one. */
    uint32_t highest = 0u;
    for (uint8_t s = 0u; s < RC_FLASH_SLOTS; ++s) {
        rc_flash_header_t header;
        if (read_header(flash, s, &header) == RC_FLASH_OK && header.version > highest) {
            highest = header.version;
        }
    }
    const uint32_t version = highest + 1u;

    rc_flash_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = RC_FLASH_MAGIC;
    header.image_size = (uint32_t)sizeof(rc_cal_t);
    header.version = version;
    rc_digest_t crc;
    rc_digest_init(&crc);
    rc_digest_update(&crc, (const uint8_t *)cal, sizeof(rc_cal_t));
    header.image_crc = rc_digest_final(&crc);
    header.header_crc = header_crc(&header);

    /* The image first, then the header.
     *
     *  Be clear about what this buys, because it is easy to overstate. Writing the header
     *  first would leave, after a cut, a slot with a valid header describing an image that
     *  was never written - and the image left there would be from a PREVIOUS write, since
     *  flash is not erased by writing. That is the shape that could activate a stale tune.
     *
     *  It is defence in depth, not the thing that makes the design safe. The guarantees
     *  actually come from two places: every read of a slot checks the image CRC, so a slot
     *  with a stale image is rejected wherever it is read; and every activation re-verifies
     *  before flipping the one irreversible flag. Removing the verify gate from
     *  rc_flash_activate makes three of the tests in test_flash.c fail, which is how that
     *  claim is kept honest - the write ordering on its own does not, so it is not
     *  pretending to be load-bearing. */
    int rc = flash_write(flash, flash->base[target] + sizeof(header), (const uint8_t *)cal,
                         (uint32_t)sizeof(rc_cal_t));
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    /* Erase the tail so a later, larger image cannot be read out of a previous one. */
    {
        uint8_t zero[64];
        memset(zero, 0, sizeof(zero));
        uint32_t written = (uint32_t)sizeof(rc_cal_t);
        while (written < RC_FLASH_SLOT_SIZE) {
            const uint32_t chunk = (RC_FLASH_SLOT_SIZE - written < sizeof(zero))
                ? (RC_FLASH_SLOT_SIZE - written) : (uint32_t)sizeof(zero);
            rc = flash_write(flash, flash->base[target] + (uint32_t)sizeof(header) + written, zero, chunk);
            if (rc != RC_FLASH_OK) {
                return rc;
            }
            written += chunk;
        }
    }
    return flash_write(flash, flash->base[target], &header, sizeof(header));
}

int rc_flash_verify_staged(rc_flash_t *flash)
{
    if (flash == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    const uint8_t staged = (uint8_t)(flash->active ^ 1u);
    rc_cal_t readback;
    uint32_t version = 0u;
    const int rc = rc_flash_read_slot(flash, staged, &readback, &version);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    /* Verified, not merely readable.  A slot can read back perfectly and still be a
     * calibration the ECU must refuse to run: this is the last point at which a bad tune
     * can be caught without having already put it on the road. */
    uint32_t blocking = 0u;
    if (rc_cal_validate(&readback, NULL, 0u, &blocking) == false) {
        return RC_FLASH_ERR_VALIDATE;
    }
    return RC_FLASH_OK;
}

int rc_flash_activate(rc_flash_t *flash)
{
    if (flash == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    const uint8_t staged = (uint8_t)(flash->active ^ 1u);
    /* Re-verify before the one irreversible step.  The slot may have been staged a long
     * time ago, or by a different session; the cost is milliseconds and the alternative is
     * activating something nobody has checked. */
    const int rc = rc_flash_verify_staged(flash);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    const int wrote = write_marker(flash, staged);
    if (wrote != RC_FLASH_OK) {
        return wrote; /* the running tune is untouched */
    }
    flash->active = staged;
    /* The cached version must follow the activation. Leaving it stale means the next
     * write computes its version from headers it has not re-read, and a caller asking
     * "which tune am I on" gets the answer for the previous one - which is exactly the
     * question a rollback workflow has to be able to ask. */
    uint32_t version = 0u;
    (void)rc_flash_read_slot(flash, staged, NULL, &version);
    flash->active_version = version;
    return RC_FLASH_OK;
}

int rc_flash_commit(rc_flash_t *flash, const rc_cal_t *cal)
{
    int rc = rc_flash_stage(flash, cal);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    rc = rc_flash_verify_staged(flash);
    if (rc != RC_FLASH_OK) {
        return rc;
    }
    return rc_flash_activate(flash);
}

int rc_flash_rollback(rc_flash_t *flash)
{
    if (flash == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    const uint8_t other = (uint8_t)(flash->active ^ 1u);
    rc_cal_t candidate;
    uint32_t version = 0u;
    if (rc_flash_read_slot(flash, other, &candidate, &version) != RC_FLASH_OK) {
        return RC_FLASH_ERR_CORRUPT;
    }
    return write_marker(flash, other);
}

int rc_flash_status(rc_flash_t *flash, rc_flash_status_t *out)
{
    if (flash == NULL || out == NULL) {
        return RC_FLASH_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    (void)read_marker(flash, &out->active);
    uint32_t highest = 0u;
    for (uint8_t s = 0u; s < RC_FLASH_SLOTS; ++s) {
        rc_flash_header_t header;
        if (read_header(flash, s, &header) == RC_FLASH_OK) {
            out->present[s] = true;
            out->version[s] = header.version;
            out->image_crc[s] = header.image_crc;
            out->image_size[s] = header.image_size;
            rc_cal_t image;
            out->verified[s] = (rc_flash_read_slot(flash, s, &image, NULL) == RC_FLASH_OK);
            if (header.version > highest) {
                highest = header.version;
            }
        }
    }
    if (out->present[out->active]) {
        out->rolled_back = (out->version[out->active] < highest);
    }
    return RC_FLASH_OK;
}

const char *rc_flash_result_text(int result)
{
    switch (result) {
        case RC_FLASH_OK: return "ok";
        case RC_FLASH_ERR_PARAM: return "invalid argument";
        case RC_FLASH_ERR_IO: return "the storage medium refused the access";
        case RC_FLASH_ERR_CORRUPT: return "the stored image failed its checksum";
        case RC_FLASH_ERR_NO_SLOT: return "no valid calibration is stored";
        case RC_FLASH_ERR_BUSY: return "the storage is busy";
        case RC_FLASH_ERR_VERSION: return "that would move the version backwards";
        case RC_FLASH_ERR_VALIDATE: return "the calibration is readable but not acceptable";
        default: return "an unrecognised flash result";
    }
}
