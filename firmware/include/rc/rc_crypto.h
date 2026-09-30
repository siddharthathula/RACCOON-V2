/* RACCOON V2 - cryptography for the tuning link.
 *
 * WHAT IS HERE AND WHAT IS NOT, AND WHY
 *
 * Here: SHA-256 and HMAC-SHA256, implemented in full, tested against the published test
 * vectors. These are the primitives the STM32 needs, and they are small enough to be
 * completely verifiable - which matters, because a home-grown hash is a hash nobody else
 * will ever check.
 *
 * Not here: RSA signature verification. The H733 has no crypto accelerator, and a
 * software RSA-3072 verify on a 480 MHz core inside a 1.25 ms per-cylinder budget is not
 * a thing to spend on it. The ESP32-S3 has hardware RSA and holds the private key in
 * eFuse, so IT verifies the signature, and it tells the STM32 the result over a link that
 * the two of them authenticate. The STM32 therefore trusts the ESP32's verdict, and the
 * trust is only as good as that link - which is why the token below is computed over the
 * actual bytes being approved, and why the STM32 re-checks the calibration rules itself
 * rather than trusting the signature to mean "safe".
 *
 * The division of labour that follows from this, and which is the actual security design:
 *
 *   - a signature proves WHERE a calibration came from, and is checked once, on the ESP32;
 *   - a rule check proves the calibration is SAFE, and is done on the STM32 every time;
 *   - an HMAC proves the STM32 and ESP32 are talking to each other, over every frame.
 *
 * None of these substitutes for another. A validly signed tune can still be lethal if the
 * injector size is wrong, so the signature is not permission to run something the rules
 * forbid; and a rule-valid tune from an unknown source is not trusted just because it
 * passes validation.
 */
#ifndef RC_CRYPTO_H
#define RC_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_SHA256_DIGEST 32u
#define RC_SHA256_BLOCK 64u
#define RC_HMAC_SHA256_DIGEST 32u

/** One-shot SHA-256. */
void rc_sha256(const void *data, size_t len, uint8_t out[RC_SHA256_DIGEST]);

/** Incremental SHA-256, for hashing a calibration without copying it. */
typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[RC_SHA256_BLOCK];
    uint32_t used;
} rc_sha256_t;

void rc_sha256_init(rc_sha256_t *ctx);
void rc_sha256_update(rc_sha256_t *ctx, const void *data, size_t len);
void rc_sha256_final(rc_sha256_t *ctx, uint8_t out[RC_SHA256_DIGEST]);

/** HMAC-SHA256 over a single buffer. */
void rc_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                    uint8_t out[RC_HMAC_SHA256_DIGEST]);

/** Constant-time comparison. Every security comparison in this codebase uses it: an
 *  early-exit compare on a MAC leaks its prefix through timing, which on a link an
 *  attacker can measure is a working forgery oracle. */
bool rc_ct_equal(const void *a, const void *b, size_t len);

/** A short, human-typeable unlock code. NOT a cryptographic secret.
 *
 *  This exists to stop someone walking up to a parked motorcycle and flashing it, and for
 *  that a short code is the right size: long enough that it is not guessable in a car park,
 *  short enough that an owner can type it on a phone in the rain. It is explicitly not
 *  protection against an attacker with the device in their hands, and the code must never
 *  be described as if it were. */
void rc_unlock_code_derive(const uint8_t *salt, size_t salt_len, const char *code,
                           uint8_t out[RC_SHA256_DIGEST]);

/** Rate limiting for unlock attempts, so a four-digit code cannot be walked by a script
 *  sitting next to the bike. */
typedef struct {
    uint8_t failures;
    uint32_t locked_until_ms;
    uint32_t now_ms;
} rc_unlock_guard_t;

void rc_unlock_guard_init(rc_unlock_guard_t *guard, uint32_t now_ms);

/** @return true if the attempt is permitted. */
bool rc_unlock_guard_allow(rc_unlock_guard_t *guard, uint32_t now_ms);

/** Record a failure. */
void rc_unlock_guard_fail(rc_unlock_guard_t *guard, uint32_t now_ms);

/** Record a success, which resets the failure count. */
void rc_unlock_guard_succeed(rc_unlock_guard_t *guard);

#ifdef __cplusplus
}
#endif
#endif /* RC_CRYPTO_H */
