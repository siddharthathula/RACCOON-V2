/* RACCOON V2 - CRC-32C (Castagnoli) and the calibration image digest.
 *
 * Integrity is the last line of defence for a configuration: a scrambled calibration
 * that still passes a bounds check can command 60 degrees of advance.  Every block of
 * the image carries its own CRC and the whole image carries a second one, computed over a
 * different region of flash (docs/CALIBRATION_SPEC.md section 2).
 *
 * CRC-32C polynomial 0x1EDC6F41, reflected 0x82F63B78 - the same polynomial used by
 * iSCSI, SCTP and the STM32 hardware CRC unit, so the bootloader can use the peripheral
 * and get identical results.
 *
 * INDEPENDENTLY AUTHORED.  No CRC implementation was copied; the table is generated at
 * first use from the reflected polynomial, which is a few lines of arithmetic.
 */
#ifndef RC_CRC32_H
#define RC_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-shot CRC-32C. `crc` may be 0 to start, or a previous value to continue. */
uint32_t rc_crc32c(uint32_t crc, const void *data, size_t len);

/** One-shot CRC-32 (IEEE 802.3), for the frame protocol where the host expects it. */
uint32_t rc_crc32(uint32_t crc, const void *data, size_t len);

/** A running digest over a configuration image, so a write can be verified in blocks. */
typedef struct {
    uint32_t crc;
    uint32_t length;
} rc_digest_t;

void rc_digest_init(rc_digest_t *digest);
void rc_digest_update(rc_digest_t *digest, const void *data, size_t len);
uint32_t rc_digest_final(const rc_digest_t *digest);

#ifdef __cplusplus
}
#endif
#endif /* RC_CRC32_H */
