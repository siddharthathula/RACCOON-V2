/* RACCOON V2 - SHA-256, HMAC-SHA256 and the unlock guard.  See rc_crypto.h. */
#include "rc/rc_crypto.h"

#include <string.h>

/* FIPS 180-4. The constants are the fractional parts of the cube roots of the first 64
 * primes, so there is nothing to mis-transcribe - but a typo here is silent and fatal, so
 * they are checked against the published vectors in the tests rather than trusted. */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(uint32_t state[8], const uint8_t block[RC_SHA256_BLOCK])
{
    uint32_t w[64];
    for (uint32_t i = 0u; i < 16u; ++i) {
        w[i] = ((uint32_t)block[i * 4u] << 24) | ((uint32_t)block[i * 4u + 1u] << 16)
            | ((uint32_t)block[i * 4u + 2u] << 8) | (uint32_t)block[i * 4u + 3u];
    }
    for (uint32_t i = 16u; i < 64u; ++i) {
        const uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u)
            ^ (w[i - 15u] >> 3u);
        const uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (uint32_t i = 0u; i < 64u; ++i) {
        const uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + s1 + ch + K[i] + w[i];
        const uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void rc_sha256_init(rc_sha256_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0u;
    ctx->used = 0u;
    memset(ctx->block, 0, sizeof(ctx->block));
}

void rc_sha256_update(rc_sha256_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL) {
        return;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (bytes == NULL) {
        return;
    }
    ctx->length += (uint64_t)len;
    while (len > 0u) {
        const uint32_t room = RC_SHA256_BLOCK - ctx->used;
        const uint32_t take = (len < (size_t)room) ? (uint32_t)len : room;
        memcpy(&ctx->block[ctx->used], bytes, take);
        ctx->used += take;
        bytes += take;
        len -= take;
        if (ctx->used == RC_SHA256_BLOCK) {
            sha256_block(ctx->state, ctx->block);
            ctx->used = 0u;
        }
    }
}

void rc_sha256_final(rc_sha256_t *ctx, uint8_t out[RC_SHA256_DIGEST])
{
    if (ctx == NULL || out == NULL) {
        return;
    }
    const uint64_t bits = ctx->length * 8u;
    static const uint8_t pad = 0x80u;
    rc_sha256_update(ctx, &pad, 1u);
    static const uint8_t zero = 0x00u;
    while (ctx->used != (RC_SHA256_BLOCK - 8u)) {
        rc_sha256_update(ctx, &zero, 1u);
    }
    uint8_t length_bytes[8];
    for (uint8_t i = 0u; i < 8u; ++i) {
        length_bytes[i] = (uint8_t)((bits >> (56u - (8u * i))) & 0xFFu);
    }
    /* Written into the context directly: rc_sha256_update would count these bytes into
     * the length, which has already been captured above. */
    memcpy(&ctx->block[ctx->used], length_bytes, 8u);
    sha256_block(ctx->state, ctx->block);
    ctx->used = 0u;

    for (uint8_t i = 0u; i < 8u; ++i) {
        out[i * 4u] = (uint8_t)((ctx->state[i] >> 24) & 0xFFu);
        out[i * 4u + 1u] = (uint8_t)((ctx->state[i] >> 16) & 0xFFu);
        out[i * 4u + 2u] = (uint8_t)((ctx->state[i] >> 8) & 0xFFu);
        out[i * 4u + 3u] = (uint8_t)(ctx->state[i] & 0xFFu);
    }
}

void rc_sha256(const void *data, size_t len, uint8_t out[RC_SHA256_DIGEST])
{
    rc_sha256_t ctx;
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, data, len);
    rc_sha256_final(&ctx, out);
}

void rc_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                    uint8_t out[RC_HMAC_SHA256_DIGEST])
{
    if (out == NULL) {
        return;
    }
    uint8_t pad[RC_SHA256_BLOCK];
    uint8_t inner[RC_SHA256_DIGEST];
    uint8_t shortened[RC_SHA256_DIGEST];

    memset(pad, 0, sizeof(pad));
    if (key_len > RC_SHA256_BLOCK) {
        /* A key longer than the block is hashed first, per RFC 2104. */
        rc_sha256(key, key_len, shortened);
        memcpy(pad, shortened, RC_SHA256_DIGEST);
    } else if (key != NULL && key_len > 0u) {
        memcpy(pad, key, key_len);
    }

    uint8_t ipad[RC_SHA256_BLOCK];
    uint8_t opad[RC_SHA256_BLOCK];
    for (uint32_t i = 0u; i < RC_SHA256_BLOCK; ++i) {
        ipad[i] = (uint8_t)(pad[i] ^ 0x36u);
        opad[i] = (uint8_t)(pad[i] ^ 0x5Cu);
    }

    rc_sha256_t ctx;
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, ipad, sizeof(ipad));
    rc_sha256_update(&ctx, data, data_len);
    rc_sha256_final(&ctx, inner);

    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, opad, sizeof(opad));
    rc_sha256_update(&ctx, inner, RC_SHA256_DIGEST);
    rc_sha256_final(&ctx, out);

    memset(pad, 0, sizeof(pad));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
    memset(shortened, 0, sizeof(shortened));
}

bool rc_ct_equal(const void *a, const void *b, size_t len)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0u;
    for (size_t i = 0u; i < len; ++i) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    /* No early exit anywhere above: the loop always runs to completion, so the time taken
     * does not depend on where the first difference was. */
    return (diff == 0u);
}

void rc_unlock_code_derive(const uint8_t *salt, size_t salt_len, const char *code,
                           uint8_t out[RC_SHA256_DIGEST])
{
    if (out == NULL) {
        return;
    }
    rc_sha256_t ctx;
    rc_sha256_init(&ctx);
    rc_sha256_update(&ctx, "RACCOON-UNLOCK-V1", 17u);
    if (salt != NULL) {
        rc_sha256_update(&ctx, salt, salt_len);
    }
    if (code != NULL) {
        rc_sha256_update(&ctx, code, strlen(code));
    }
    rc_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------ unlock guard */

/* Backoff: the wait doubles with each failure and is capped. A four-digit code is 10000
 * possibilities, so without this a script beside the bike finds it in seconds - and with
 * it, the same script needs longer than anyone will wait. */
static const uint32_t RC_UNLOCK_BASE_MS = 2000u;
static const uint32_t RC_UNLOCK_MAX_MS = 300000u; /* five minutes */

void rc_unlock_guard_init(rc_unlock_guard_t *guard, uint32_t now_ms)
{
    if (guard == NULL) {
        return;
    }
    memset(guard, 0, sizeof(*guard));
    guard->now_ms = now_ms;
}

bool rc_unlock_guard_allow(rc_unlock_guard_t *guard, uint32_t now_ms)
{
    if (guard == NULL) {
        return false;
    }
    guard->now_ms = now_ms;
    return ((int32_t)(now_ms - guard->locked_until_ms) >= 0);
}

void rc_unlock_guard_fail(rc_unlock_guard_t *guard, uint32_t now_ms)
{
    if (guard == NULL) {
        return;
    }
    guard->now_ms = now_ms;
    if (guard->failures < 255u) {
        guard->failures++;
    }
    uint32_t wait = RC_UNLOCK_BASE_MS;
    for (uint8_t i = 1u; i < guard->failures; ++i) {
        wait *= 2u;
        if (wait >= RC_UNLOCK_MAX_MS) {
            wait = RC_UNLOCK_MAX_MS;
            break;
        }
    }
    guard->locked_until_ms = now_ms + wait;
}

void rc_unlock_guard_succeed(rc_unlock_guard_t *guard)
{
    if (guard == NULL) {
        return;
    }
    guard->failures = 0u;
    guard->locked_until_ms = 0u;
}
