/* RACCOON V2 - cryptography tests.
 *
 * These are the published FIPS 180-4 and RFC 4231 vectors, not vectors this code produced
 * itself. A hash tested only against itself is a hash that agrees with whatever bug it has,
 * and the whole point of implementing it here rather than calling a library is that it can
 * be checked by someone else - which is only true if the expected values came from outside.
 */
#include "rc_test.h"

#include "rc/rc_crypto.h"

#include <stdio.h>
#include <string.h>

static void check_digest(const char *label, const uint8_t *got, const char *expected_hex)
{
    char hex[RC_SHA256_DIGEST * 2u + 1u];
    for (uint8_t i = 0u; i < RC_SHA256_DIGEST; ++i) {
        (void)snprintf(&hex[i * 2u], 3u, "%02x", (unsigned)got[i]);
    }
    RC_CHECK_MSG(strcmp(hex, expected_hex) == 0, "%s: got %s, expected %s", label, hex,
                 expected_hex);
}

/* FIPS 180-4 examples. */
static void sha256_matches_the_published_vectors(void)
{
    uint8_t digest[RC_SHA256_DIGEST];

    rc_sha256("", 0u, digest);
    check_digest("empty", digest,
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    rc_sha256("abc", 3u, digest);
    check_digest("abc", digest,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    rc_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, digest);
    check_digest("448-bit", digest,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a' characters: the classic long-message vector, and the one that finds
     * length-encoding bugs the three above never reach. */
    rc_sha256_t ctx;
    rc_sha256_init(&ctx);
    static uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; ++i) {
        rc_sha256_update(&ctx, chunk, sizeof(chunk));
    }
    rc_sha256_final(&ctx, digest);
    check_digest("one million a", digest,
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* Feeding the same bytes in one call and in many must give the same digest. This is the
 * property the calibration-image hashing depends on, and it is the one a buffered
 * implementation gets wrong. */
static void incremental_hashing_matches_one_shot(void)
{
    uint8_t data[257];
    for (size_t i = 0u; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 31u + (i >> 3u));
    }
    uint8_t one_shot[RC_SHA256_DIGEST];
    rc_sha256(data, sizeof(data), one_shot);

    for (size_t split = 0u; split <= sizeof(data); ++split) {
        rc_sha256_t ctx;
        uint8_t incremental[RC_SHA256_DIGEST];
        rc_sha256_init(&ctx);
        rc_sha256_update(&ctx, data, split);
        rc_sha256_update(&ctx, &data[split], sizeof(data) - split);
        rc_sha256_final(&ctx, incremental);
        RC_CHECK_MSG(memcmp(one_shot, incremental, RC_SHA256_DIGEST) == 0,
                     "a split at %u bytes gave a different digest", (unsigned)split);
    }
    /* And one byte at a time, which is what a slow bus feeding a calibration does. */
    rc_sha256_t ctx;
    uint8_t byte_at_a_time[RC_SHA256_DIGEST];
    rc_sha256_init(&ctx);
    for (size_t i = 0u; i < sizeof(data); ++i) {
        rc_sha256_update(&ctx, &data[i], 1u);
    }
    rc_sha256_final(&ctx, byte_at_a_time);
    RC_CHECK_EQ_U(memcmp(one_shot, byte_at_a_time, RC_SHA256_DIGEST), 0u);
}

/* Messages either side of the 64-byte block boundary, where padding goes wrong. */
static void padding_is_correct_at_the_block_boundaries(void)
{
    /* Generated with an independent implementation, NOT by this code: a boundary vector
     * written from memory tests nothing but the writer's memory. */
    struct {
        size_t len;
        const char *expected;
    } cases[] = {
        {55u, "595615dbe4f0f407ae397d08b4c2cb870cb9b0e11937416f950c5160acf9c005"},
        {56u, "784f623b787495078e93ff28a25b581df0584055a7e71d8cd90c454716b92f51"},
        {63u, "5ca3e1ef5207490eac01a795e5cc94d59582a5118bf9534665c8668d87aa647c"},
        {64u, "2fcd5a0d60e4c941381fcc4e00a4bf8be422c3ddfafb93c809e8d1e2bfffae8e"},
        {65u, "1b3cd1877ab2f2f19f7be001722554f336cb799df0329de0bb4c118dc6abc06d"},
        {119u, "faef67da856d6fd9c8d12f9ed0a4fefd3cf0ce085ab43e2907418d457e3c354b"},
        {120u, "c9512b08619c19fbb503c7da6b46ef20301e5f7a7a5f43989182398536f5c5c8"},
        {127u, "1b9907986f18ddc2b37e18be502d82d69d6e18bb9dff9e16f2aceea7d88cb933"}
    };
    uint8_t data[128];
    for (size_t i = 0u; i < sizeof(data); ++i) {
        data[i] = (uint8_t)('a' + (i % 26u));
    }
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t digest[RC_SHA256_DIGEST];
        rc_sha256(data, cases[i].len, digest);
        check_digest("block boundary", digest, cases[i].expected);
    }
}

/* RFC 4231 test cases 1, 2, 3 and 6. */
static void hmac_matches_the_published_vectors(void)
{
    uint8_t key[131];
    uint8_t mac[RC_HMAC_SHA256_DIGEST];
    /* 131 bytes, as RFC 4231 case 6 specifies. Written by count rather than by eye: a
     * key that is one byte short still produces a perfectly good HMAC, just not the one
     * the vector says it should be. */
    uint8_t long_key[131];
    memset(long_key, 0xaa, sizeof(long_key));

    /* Case 1: 20 bytes of 0x0b, data "Hi There". */
    memset(key, 0x0b, 20u);
    rc_hmac_sha256(key, 20u, "Hi There", 8u, mac);
    check_digest("rfc4231-1", mac,
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    /* Case 2: key "Jefe", data "what do ya want for nothing?". */
    rc_hmac_sha256("Jefe", 4u, "what do ya want for nothing?", 28u, mac);
    check_digest("rfc4231-2", mac,
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* Case 3: 20 bytes of 0xaa, data 50 bytes of 0xdd. */
    memset(key, 0xaa, 20u);
    uint8_t data[50];
    memset(data, 0xdd, sizeof(data));
    rc_hmac_sha256(key, 20u, data, sizeof(data), mac);
    check_digest("rfc4231-3", mac,
                 "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    /* Case 6: a key longer than the block, which must be hashed first. */
    rc_hmac_sha256(long_key, sizeof(long_key), "Test Using Larger Than Block-Size Key - Hash Key First",
                   54u, mac);
    check_digest("rfc4231-6", mac,
                 "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

/* A MAC must change completely when one bit of the message changes. */
static void a_mac_covers_every_bit_of_its_message(void)
{
    uint8_t key[32];
    memset(key, 0x5A, sizeof(key));
    uint8_t message[64];
    memset(message, 0x00, sizeof(message));
    uint8_t baseline[RC_HMAC_SHA256_DIGEST];
    rc_hmac_sha256(key, sizeof(key), message, sizeof(message), baseline);
    for (size_t bit = 0u; bit < sizeof(message) * 8u; ++bit) {
        message[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        uint8_t mac[RC_HMAC_SHA256_DIGEST];
        rc_hmac_sha256(key, sizeof(key), message, sizeof(message), mac);
        RC_CHECK_MSG(memcmp(baseline, mac, RC_HMAC_SHA256_DIGEST) != 0,
                     "a MAC did not change with bit %u flipped", (unsigned)bit);
        message[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
}

/* The constant-time compare must be correct, and must not be written as a loop with an
 * early exit - which is checked here by reading for it rather than by timing, because a
 * timing test on a host compiler proves nothing about the target. */
static void constant_time_compare_is_correct(void)
{
    uint8_t a[32];
    uint8_t b[32];
    memset(a, 0x11, sizeof(a));
    memcpy(b, a, sizeof(b));
    RC_CHECK(rc_ct_equal(a, b, sizeof(a)));
    /* Differing at the first byte. */
    b[0] = 0x12;
    RC_CHECK(!rc_ct_equal(a, b, sizeof(a)));
    /* Differing at the last byte, which an early-exit compare would take longest to find. */
    memcpy(b, a, sizeof(b));
    b[31] = 0x12;
    RC_CHECK(!rc_ct_equal(a, b, sizeof(a)));
    /* Differing only in length-relevant trailing bytes. */
    RC_CHECK(!rc_ct_equal(a, NULL, sizeof(a)));
    RC_CHECK(!rc_ct_equal(NULL, b, sizeof(a)));
    RC_CHECK(rc_ct_equal(a, a, 0u));
}

/* The unlock backoff must actually stop a script. */
static void unlock_backoff_grows_and_locks_out(void)
{
    rc_unlock_guard_t guard;
    rc_unlock_guard_init(&guard, 1000u);
    RC_CHECK(rc_unlock_guard_allow(&guard, 1000u));

    rc_unlock_guard_fail(&guard, 1000u);
    /* Immediately refused. */
    RC_CHECK(!rc_unlock_guard_allow(&guard, 1001u));
    /* And refused until the backoff expires. */
    RC_CHECK(!rc_unlock_guard_allow(&guard, 2000u));
    RC_CHECK(rc_unlock_guard_allow(&guard, 4000u));

    /* Repeated failures push it out further, and it is capped rather than wrapping. */
    /* Step forward by less than the longest possible backoff each time, so the lock is
     * still in force when it is checked. Stepping by more than the cap would look like the
     * backoff had stopped working when in fact it had simply expired. */
    uint32_t now = 4000u;
    for (int i = 0; i < 20; ++i) {
        rc_unlock_guard_fail(&guard, now);
        now += 1000u;
    }
    RC_CHECK_MSG(!rc_unlock_guard_allow(&guard, now), "the backoff stopped applying");
    /* Wrapping the clock must not unlock it early. */
    RC_CHECK(!rc_unlock_guard_allow(&guard, 0u));

    /* A correct code clears the state. */
    rc_unlock_guard_succeed(&guard);
    RC_CHECK(rc_unlock_guard_allow(&guard, now));
}

/* The derived unlock value must depend on the salt, so two devices with the same code do
 * not share a stored value. */
static void unlock_derivation_is_salted(void)
{
    uint8_t salt_a[8];
    uint8_t salt_b[8];
    memset(salt_a, 0xA1, sizeof(salt_a));
    memset(salt_b, 0xB2, sizeof(salt_b));
    uint8_t from_a[RC_SHA256_DIGEST];
    uint8_t from_b[RC_SHA256_DIGEST];
    rc_unlock_code_derive(salt_a, sizeof(salt_a), "1234", from_a);
    rc_unlock_code_derive(salt_b, sizeof(salt_b), "1234", from_b);
    RC_CHECK_MSG(memcmp(from_a, from_b, RC_SHA256_DIGEST) != 0,
                 "two different devices derived the same unlock value");
    /* And the same salt and code must derive the same value, or the owner cannot get in. */
    uint8_t again[RC_SHA256_DIGEST];
    rc_unlock_code_derive(salt_a, sizeof(salt_a), "1234", again);
    RC_CHECK_EQ_U(memcmp(from_a, again, RC_SHA256_DIGEST), 0u);
    /* A different code must give a different value. */
    uint8_t other[RC_SHA256_DIGEST];
    rc_unlock_code_derive(salt_a, sizeof(salt_a), "1235", other);
    RC_CHECK(memcmp(from_a, other, RC_SHA256_DIGEST) != 0);
}

void test_crypto(void)
{
    RC_TEST_CASE("sha256/matches/the/published/vectors",
                 sha256_matches_the_published_vectors);
    RC_TEST_CASE("incremental/hashing/matches/one/shot",
                 incremental_hashing_matches_one_shot);
    RC_TEST_CASE("padding/is/correct/at/the/block/boundaries",
                 padding_is_correct_at_the_block_boundaries);
    RC_TEST_CASE("hmac/matches/the/published/vectors", hmac_matches_the_published_vectors);
    RC_TEST_CASE("a/mac/covers/every/bit/of/its/message", a_mac_covers_every_bit_of_its_message);
    RC_TEST_CASE("constant/time/compare/is/correct", constant_time_compare_is_correct);
    RC_TEST_CASE("unlock/backoff/grows/and/locks/out", unlock_backoff_grows_and_locks_out);
    RC_TEST_CASE("unlock/derivation/is/salted", unlock_derivation_is_salted);
}
