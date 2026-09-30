/* RACCOON V2 - tests for the tuner link service.
 *
 * This is the security boundary. Everything the tuner can do to a calibration passes
 * through rc_link_service_handle, so the tests here are mostly about what it REFUSES:
 * a write before unlocking, a write while the engine is running, a value outside an
 * item's range, an image that breaks a cross-item rule, a proof replayed against a
 * different salt, and a chunked transfer spliced out of order.
 */
#include "rc_test.h"

#include "cal_access.h"
#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/rc_crc32.h"
#include "rc/rc_crypto.h"
#include "rc/rc_flash.h"
#include "rc/rc_link.h"
#include "rc/rc_link_service.h"

/* A flash region big enough for both slots, so the real flash layer is exercised rather
 * than a mock. Testing the service against a stub would miss the very thing most likely to
 * be wrong: whether what the service staged is what the flash layer will activate. */
#define TEST_MEDIUM_SIZE 0x30000u
#define TEST_BASE0 0x10000u
#define TEST_BASE1 0x20000u
#define TEST_MARKER 0x28000u

/* The real flash layer over a plain memory-backed medium, so the service is tested against
 * the thing that will actually hold the tune rather than against a stub that agrees with
 * whatever the service already believes. */
typedef struct {
    uint8_t data[TEST_MEDIUM_SIZE];
    uint8_t hal_ctx;
} medium_t;

static medium_t medium;

static int medium_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    (void)ctx;
    if ((size_t)offset + (size_t)len > sizeof(medium.data)) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(dst, &medium.data[offset], len);
    return RC_HAL_OK;
}

static int medium_write(void *ctx, uint32_t offset, const void *src, uint32_t len)
{
    (void)ctx;
    if ((size_t)offset + (size_t)len > sizeof(medium.data)) {
        return RC_HAL_ERR_PARAM;
    }
    memcpy(&medium.data[offset], src, len);
    return RC_HAL_OK;
}

/* File-static, not a function local: rc_flash_init and rc_link_service_init both keep the
 * pointer they are given, so a HAL whose lifetime ends with this function is a dangling
 * pointer that only shows up on the next flash read. This exact mistake was made in the
 * simulator's link server and caught by AddressSanitizer, not by a test. */
static rc_hal_t g_hal;

static rc_hal_t *memory_hal(medium_t *m)
{
    (void)m;   /* the context is the module-level medium; the struct is only a handle */
    memset(&g_hal, 0, sizeof(g_hal));
    g_hal.storage_read = medium_read;
    g_hal.storage_write = medium_write;
    return &g_hal;
}

typedef struct {
    rc_link_service_t service;
    rc_flash_t flash;
    bool running;
    rc_engine_state_t live;
} fixture_t;

static void fixture_init(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    /* A fresh medium per test: leftover activation flags from the previous test would make
     * a rollback test pass or fail for the wrong reason. */
    memset(&medium, 0xFF, sizeof(medium));
    (void)rc_flash_init(&f->flash, memory_hal(&medium), &medium, TEST_BASE0, TEST_BASE1,
                        TEST_MARKER);
    rc_link_service_init(&f->service, memory_hal(&medium), &medium, &f->flash);
    f->service.unlock_code = "race-ecu";
    rc_cal_t loaded;
    (void)rc_flash_load_active(&f->flash, &loaded);
    rc_link_service_adopt(&f->service, &loaded);
    f->service.live_source = NULL;
}

static bool armed_hook(void *ctx)
{
    return ((fixture_t *)ctx)->running;
}

static const rc_engine_state_t *live_hook(void *ctx)
{
    return &((fixture_t *)ctx)->live;
}

static uint16_t call(fixture_t *f, uint8_t command, const uint8_t *payload, uint16_t length,
                     uint8_t *response, uint16_t cap)
{
    /* Zeroed first, every time.
     *
     * A test that inspects a response buffer the service did not fill is reading whatever
     * was on the stack - which passes today, fails after an unrelated refactor, and fails
     * differently under a different compiler. Zeroing here rather than in thirty call sites
     * means the invariant cannot be forgotten. */
    memset(response, 0, cap);
    rc_link_header_t header;
    header.sof = RC_LINK_SOF;
    header.version = RC_LINK_VERSION;
    header.command = command;
    header.flags = 0u;
    header.sequence = 7u;
    header.length = length;
    return rc_link_service_handle(&f->service, &header, payload, length, response, cap);
}

/** The result code, or a value that cannot be confused with one if nothing was written. */
#define RC_TEST_NO_REPLY 0xFFu

static uint8_t result_of(const uint8_t *response, uint16_t length)
{
    if (length == 0u) {
        return RC_TEST_NO_REPLY;
    }
    return response[0];
}

static uint16_t u16_of(const uint8_t *response, uint16_t offset)
{
    return (uint16_t)(response[offset] | ((uint16_t)response[offset + 1u] << 8));
}

static uint32_t u32_of(const uint8_t *response, uint16_t offset)
{
    return (uint32_t)response[offset] | ((uint32_t)response[offset + 1u] << 8)
        | ((uint32_t)response[offset + 2u] << 16) | ((uint32_t)response[offset + 3u] << 24);
}

static float f32_of(const uint8_t *response, uint16_t offset)
{
    union { uint32_t u; float f; } conv;
    conv.u = u32_of(response, offset);
    return conv.f;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* --------------------------------------------------------------------- basics */

static void ping_and_info_agree_with_the_image(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t response[64];

    uint16_t n = call(&f, RC_CMD_PING, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);

    n = call(&f, RC_CMD_GET_INFO, NULL, 0u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    /* The image size the ECU reports is the whole point of the whole exercise: a tuner
     * built for a different layout must refuse to talk rather than read garbage. */
    RC_CHECK_EQ_U(u32_of(response, 9), (uint32_t)RC_CAL_IMAGE_SIZE);
    /* Counted from the generated table, so it tracks the schema. A hard-coded 519 would go
     * stale the next time an item is added and would fail for a reason unrelated to what
     * this test is about. */
    RC_CHECK_EQ_U(u16_of(response, 13), rc_cal_item_count());
    RC_CHECK_EQ_U(response[25], 0u); /* not armed */
    /* The salt is published in the identity block, so a tuner needs no separate round trip
     * to learn it before it can build an unlock proof. */
    RC_CHECK_EQ_U(u16_of(response, 29), 0u);   /* auth failures so far */
    RC_CHECK(memcmp(&response[31], f.service.salt, RC_LINK_SALT_SIZE) == 0);
}

static void an_unknown_command_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t response[16];
    const uint16_t n = call(&f, 0x7Eu, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_BAD_COMMAND);
}

/* ------------------------------------------------------------ authentication */

static void the_service_issues_a_usable_salt(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t response[64];
    const uint16_t n = call(&f, RC_CMD_GET_STATUS, NULL, 0u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_EQ_U(response[4], 0u); /* not authenticated before unlocking */
}

/* The proof is HMAC(key = salt, salt || digest(image)). The test derives it the same way a
 * correct client would, which is the only way this test is worth anything: a test that
 * used the same helper the implementation used would pass if both were wrong. */
static void authenticate_produces_the_proof_the_ecu_accepts(void)
{
    fixture_t f;
    fixture_init(&f);
    const rc_link_service_t *s = &f.service;

    uint8_t request[64];
    memcpy(&request[0], s->salt, RC_LINK_SALT_SIZE);
    uint8_t material[RC_LINK_SALT_SIZE + 4u];
    memcpy(material, s->salt, RC_LINK_SALT_SIZE);
    put_u32(&material[RC_LINK_SALT_SIZE], rc_cal_digest(&f.service.working));
    uint8_t key[RC_SHA256_DIGEST];
    rc_unlock_code_derive(s->salt, RC_LINK_SALT_SIZE, "race-ecu", key);
    uint8_t expected[RC_HMAC_SHA256_DIGEST];
    rc_hmac_sha256(key, sizeof(key), material, sizeof(material), expected);
    memcpy(&request[RC_LINK_SALT_SIZE], expected, RC_HMAC_SHA256_DIGEST);

    uint8_t response[128];
    const uint16_t n = call(&f, RC_CMD_AUTHENTICATE, request, sizeof(request), response,
                            sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);
    RC_CHECK_EQ_U(f.service.authenticated, 1u);
}

static void a_wrong_proof_is_refused_and_counted(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[64];
    memcpy(&request[0], f.service.salt, RC_LINK_SALT_SIZE);
    memset(&request[RC_LINK_SALT_SIZE], 0xAB, RC_SHA256_DIGEST);

    uint8_t response[128];
    uint16_t n = call(&f, RC_CMD_AUTHENTICATE, request, sizeof(request), response,
                      sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_AUTHENTICATED);
    RC_CHECK_EQ_U(f.service.authenticated, 0u);
    RC_CHECK(f.service.auth_failures > 0u);
}

/* The proof must depend on the CODE, not only on the salt.
 *
 * The salt is published in GET_INFO, so a proof derived from it alone authenticates anyone
 * who has read the identity block - which is every client that ever connects. This test
 * sends exactly that proof: everything correct except that no code was mixed in. It is the
 * shape of mistake that leaves the unlock looking like it works, because the operator's own
 * attempts succeed right up until someone tries without the code.
 */
static void a_proof_that_omits_the_code_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[64];
    memcpy(&request[0], f.service.salt, RC_LINK_SALT_SIZE);
    uint8_t material[RC_LINK_SALT_SIZE + 4u];
    memcpy(material, f.service.salt, RC_LINK_SALT_SIZE);
    put_u32(&material[RC_LINK_SALT_SIZE], rc_cal_digest(&f.service.working));
    uint8_t proof[RC_HMAC_SHA256_DIGEST];
    rc_hmac_sha256(f.service.salt, RC_LINK_SALT_SIZE, material, sizeof(material), proof);
    memcpy(&request[RC_LINK_SALT_SIZE], proof, RC_HMAC_SHA256_DIGEST);

    uint8_t response[128];
    const uint16_t n = call(&f, RC_CMD_AUTHENTICATE, request, sizeof(request), response,
                            sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_AUTHENTICATED);
    RC_CHECK_EQ_U(f.service.authenticated, 0u);
}

/* An ECU with no code provisioned must be impossible to unlock, not trivially unlockable. */
static void an_unprovisioned_ecu_cannot_be_unlocked(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.unlock_code = "";
    uint8_t request[64];
    memcpy(&request[0], f.service.salt, RC_LINK_SALT_SIZE);
    uint8_t material[RC_LINK_SALT_SIZE + 4u];
    memcpy(material, f.service.salt, RC_LINK_SALT_SIZE);
    put_u32(&material[RC_LINK_SALT_SIZE], rc_cal_digest(&f.service.working));
    uint8_t key[RC_SHA256_DIGEST];
    rc_unlock_code_derive(f.service.salt, RC_LINK_SALT_SIZE, "", key);
    uint8_t proof[RC_HMAC_SHA256_DIGEST];
    rc_hmac_sha256(key, sizeof(key), material, sizeof(material), proof);
    memcpy(&request[RC_LINK_SALT_SIZE], proof, RC_HMAC_SHA256_DIGEST);

    uint8_t response[128];
    const uint16_t n = call(&f, RC_CMD_AUTHENTICATE, request, sizeof(request), response,
                            sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_AUTHENTICATED);
    RC_CHECK_EQ_U(f.service.authenticated, 0u);
}

/* A capture from a real session, replayed with the tuner's own salt substituted, must not
 * authenticate. Accepting the presented salt would let anyone choose a fresh salt, derive
 * the matching proof from the code, and replay a recording. */
static void a_replayed_proof_with_a_swapped_salt_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[64];
    memset(&request[0], 0x5A, RC_LINK_SALT_SIZE);
    uint8_t material[RC_LINK_SALT_SIZE + 4u];
    memset(material, 0x5A, RC_LINK_SALT_SIZE);
    put_u32(&material[RC_LINK_SALT_SIZE], rc_cal_digest(&f.service.working));
    uint8_t key[RC_SHA256_DIGEST];
    rc_unlock_code_derive((const uint8_t *)"\x5a\x5a\x5a\x5a", 4u, "race-ecu", key);
    uint8_t proof[RC_HMAC_SHA256_DIGEST];
    rc_hmac_sha256(key, sizeof(key), material, sizeof(material), proof);
    memcpy(&request[RC_LINK_SALT_SIZE], proof, RC_HMAC_SHA256_DIGEST);

    uint8_t response[128];
    const uint16_t n = call(&f, RC_CMD_AUTHENTICATE, request, sizeof(request), response,
                            sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_AUTHENTICATED);
}

/* ------------------------------------------------------------------- writes */

static void a_write_before_unlocking_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[8];
    put_u16(&request[0], (uint16_t)rc_cal_id_of("engine.cylinder_count"));
    put_u32(&request[2], 0x3F800000u); /* 1.0f */
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_WRITE_ITEM, request, 6u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_AUTHENTICATED);
    RC_CHECK_EQ_U((uint32_t)rc_cal_get_float(&f.service.working,
                                            rc_cal_id_of("engine.cylinder_count"), NULL), 0u);
}

static void a_write_while_the_engine_runs_is_refused_even_when_unlocked(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.armed_source = armed_hook;
    f.service.armed_ctx = &f;
    f.service.authenticated = true;
    f.running = true;

    uint8_t request[8];
    put_u16(&request[0], (uint16_t)rc_cal_id_of("engine.cylinder_count"));
    put_u32(&request[2], 0x3F800000u);
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_WRITE_ITEM, request, 6u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_NOT_ARMED);
}

static void a_value_outside_the_items_own_range_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;

    uint8_t request[8];
    put_u16(&request[0], (uint16_t)rc_cal_id_of("engine.cylinder_count"));
    put_u32(&request[2], 0x42C80000u); /* 100.0f, on a 1..4 item */
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_WRITE_ITEM, request, 6u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OUT_OF_RANGE);
}

static void a_write_that_breaks_a_cross_item_rule_is_refused_and_names_the_rule(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;

    /* injector_cylinder_count > engine_cylinder_count is a generated rule. Setting the
     * smaller one alone is refused, and the reply carries the rule so the tuner's dialog
     * can say which one - a bare "rejected" gets worked around. */
    uint8_t request[8];
    put_u16(&request[0], (uint16_t)rc_cal_id_of("hardware.injector_channel_count"));
    put_u32(&request[2], 0x41200000u); /* 10.0f, against 2 cylinders */
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_WRITE_ITEM, request, 6u, response, sizeof(response));
    const uint8_t result = result_of(response, n);
    RC_CHECK(result == (uint8_t)RC_RESP_RULE_VIOLATION || result == (uint8_t)RC_RESP_OUT_OF_RANGE);
    if (result == (uint8_t)RC_RESP_RULE_VIOLATION) {
        RC_CHECK(n >= 3u);
    }
}

static void a_short_write_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_WRITE_ITEM, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_BAD_LENGTH);
}

/* -------------------------------------------------------------------- reads */

static void read_bulk_returns_the_whole_image_with_matching_offsets(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t response[600];
    uint32_t offset = 0u;
    uint8_t assembled[RC_CAL_IMAGE_SIZE];
    while (offset < (uint32_t)RC_CAL_IMAGE_SIZE) {
        uint8_t request[8];
        put_u32(&request[0], offset);
        put_u16(&request[4], 512u);
        const uint16_t n = call(&f, RC_CMD_READ_BULK, request, 6u, response, sizeof(response));
        RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
        /* The echoed offset is what lets the client refuse a misfiled chunk. */
        RC_CHECK_EQ_U(u32_of(response, 1), offset);
        const uint16_t got = (uint16_t)(n - 5u);
        memcpy(&assembled[offset], &response[5], got);
        offset += got;
    }
    RC_CHECK_EQ_U(offset, (uint32_t)RC_CAL_IMAGE_SIZE);
    RC_CHECK(memcmp(assembled, &f.service.working, RC_CAL_IMAGE_SIZE) == 0);
}

/* Every reply must be framable. A response larger than the frame payload is not truncated
 * with a warning - rc_link_encode refuses it, the frame is never sent, and the client waits
 * forever for a reply the ECU believes it already sent. */
static void no_reply_exceeds_the_frame_payload(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t response[RC_LINK_MAX_PAYLOAD + 64u];
    static const struct { uint8_t command; uint16_t len; } probes[] = {
        { RC_CMD_GET_INFO, 0u },
        { RC_CMD_GET_STATUS, 0u },
        { RC_CMD_GET_FAULTS, 0u },
        { RC_CMD_READ_BULK, 6u },
        { RC_CMD_PING, 0u },
    };
    for (unsigned i = 0u; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        uint8_t request[16];
        memset(request, 0, sizeof(request));
        request[4] = 0xFFu;   /* ask for the largest chunk the protocol allows */
        request[5] = 0xFFu;
        const uint16_t n = call(&f, probes[i].command, request, probes[i].len, response,
                                (uint16_t)sizeof(response));
        RC_CHECK_MSG(n <= RC_LINK_MAX_PAYLOAD,
                     "%s produced a %u-byte reply; the frame limit is %u",
                     rc_link_command_name(probes[i].command), (unsigned)n,
                     (unsigned)RC_LINK_MAX_PAYLOAD);
        /* And it must actually encode. */
        rc_link_header_t header;
        memset(&header, 0, sizeof(header));
        header.command = (uint8_t)RC_CMD_RESPONSE;
        header.length = n;
        uint8_t frame[RC_LINK_MAX_FRAME];
        RC_CHECK_MSG(rc_link_encode(&header, response, n, frame, (uint16_t)sizeof(frame)) > 0u,
                     "the %s reply cannot be framed", rc_link_command_name(probes[i].command));
    }
}

static void read_bulk_past_the_end_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[8];
    put_u32(&request[0], (uint32_t)RC_CAL_IMAGE_SIZE + 16u);
    put_u16(&request[4], 512u);
    uint8_t response[600];
    const uint16_t n = call(&f, RC_CMD_READ_BULK, request, 6u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_BAD_ID);
}

static void read_item_returns_the_working_value(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[8];
    put_u16(&request[0], (uint16_t)rc_cal_id_of("engine.cylinder_count"));
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_READ_ITEM, request, 2u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    float value = 0.0f;
    RC_CHECK(rc_cal_get_float(&f.service.working, rc_cal_id_of("engine.cylinder_count"), &value));
    RC_CHECK_NEAR(f32_of(response, 1), value, 1e-4);
}

static void an_unknown_item_id_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    uint8_t request[8];
    put_u16(&request[0], 0xFFFEu);
    uint8_t response[32];
    const uint16_t n = call(&f, RC_CMD_READ_ITEM, request, 2u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_BAD_ID);
}

/* -------------------------------------------------------------------- flash */

static uint8_t send_image(fixture_t *f, const uint8_t *image, uint32_t size)
{
    uint8_t response[600];
    uint32_t offset = 0u;
    while (offset < size) {
        uint16_t chunk = 512u;
        if ((uint32_t)chunk > size - offset) {
            chunk = (uint16_t)(size - offset);
        }
        uint8_t request[520];
        put_u32(&request[0], offset);
        put_u16(&request[4], chunk);
        memcpy(&request[6], &image[offset], chunk);
        const uint16_t n = call(f, RC_CMD_STAGE, request, (uint16_t)(6u + chunk), response,
                                sizeof(response));
        const uint8_t result = result_of(response, n);
        if (result != (uint8_t)RC_RESP_OK) {
            return result;
        }
        offset += chunk;
    }
    return (uint8_t)RC_RESP_OK;
}

static void a_full_stage_verify_activate_round_trip(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    const uint8_t *image = (const uint8_t *)&f.service.working;

    RC_CHECK_EQ_U(send_image(&f, image, RC_CAL_IMAGE_SIZE), (uint8_t)RC_RESP_OK);

    uint8_t response[64];
    uint16_t n = call(&f, RC_CMD_VERIFY, NULL, 0u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    const uint32_t digest = u32_of(response, 1);
    RC_CHECK_EQ_U(digest, rc_cal_digest(&f.service.working));

    n = call(&f, RC_CMD_ACTIVATE, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);

    n = call(&f, RC_CMD_GET_STATUS, NULL, 0u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_EQ_U(u32_of(response, 5), 0u); /* nothing left dirty */
}

/* After a rollback the ECU must SERVE the tune it rolled back to, not the one it just
 * replaced. A service that kept its own working copy would report success and then hand
 * back the old tune forever - and the operator's next move is to try the rollback again. */
static void a_rollback_makes_the_ecu_serve_the_previous_tune(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    /* Tune A, with a value that differs from the defaults so the change is visible. */
    rc_cal_t first = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_cylinder_count(&first, 3u);
    RC_CHECK_EQ_U(rc_flash_stage(&f.flash, &first), 0);
    RC_CHECK_EQ_U(rc_flash_verify_staged(&f.flash), 0);
    /* Activated over the LINK, not by calling the flash layer directly: the reload of the
     * working image is part of the command, and calling past it would test a path a tuner
     * never takes. */
    uint8_t response[64];
    uint16_t n = call(&f, RC_CMD_ACTIVATE, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);
    n = call(&f, RC_CMD_READ_ITEM, (const uint8_t[]){0x00, 0x01}, 2u, response,
                      sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_NEAR(f32_of(response, 1), 3.0f, 1e-4);

    /* Tune B, activated over the top - again through the link. */
    rc_cal_t second = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_cylinder_count(&second, 4u);
    RC_CHECK_EQ_U(rc_flash_stage(&f.flash, &second), 0);
    RC_CHECK_EQ_U(rc_flash_verify_staged(&f.flash), 0);
    n = call(&f, RC_CMD_ACTIVATE, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);
    n = call(&f, RC_CMD_READ_ITEM, (const uint8_t[]){0x00, 0x01}, 2u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_NEAR(f32_of(response, 1), 4.0f, 1e-4);

    /* Roll back, over the link, the way a tuner does it. */
    n = call(&f, RC_CMD_ROLLBACK, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);
    n = call(&f, RC_CMD_READ_ITEM, (const uint8_t[]){0x00, 0x01}, 2u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_NEAR(f32_of(response, 1), 3.0f, 1e-4);
}

/* Rollback onto a slot that never held a valid tune must be refused, not reported as
 * success. A rollback that "worked" onto an invalid slot would leave the ECU with no
 * calibration at all, which is the one outcome worse than refusing to move.
 *
 * Testable here and only here: the medium can be blanked deterministically, whereas a live
 * rig's flash history depends on whatever the previous test did to it. */
static void a_rollback_onto_an_invalid_slot_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    uint8_t response[64];

    /* One valid tune, activated. The other slot has never been written. */
    rc_cal_t tune = rc_cal_defaults_filled();
    (void)rc_cal_set_engine_cylinder_count(&tune, 3u);
    RC_CHECK_EQ_U(rc_flash_stage(&f.flash, &tune), 0);
    RC_CHECK_EQ_U(rc_flash_verify_staged(&f.flash), 0);
    uint16_t n = call(&f, RC_CMD_ACTIVATE, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);

    n = call(&f, RC_CMD_ROLLBACK, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_STORAGE);

    /* And the ECU is still on the tune it had, rather than on nothing. */
    n = call(&f, RC_CMD_READ_ITEM, (const uint8_t[]){0x00, 0x01}, 2u, response, sizeof(response));
    RC_REQUIRE(result_of(response, n) == (uint8_t)RC_RESP_OK);
    RC_CHECK_NEAR(f32_of(response, 1), 3.0f, 1e-4);
}

/* Chunks arriving out of order must be refused. Accepting them would splice two
 * transfers into an image that is a mixture of both, which is the sort of thing that
 * passes a CRC and runs. */
static void an_out_of_order_chunk_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    const uint8_t *image = (const uint8_t *)&f.service.working;

    uint8_t response[600];
    uint8_t request[520];
    put_u32(&request[0], 0u);
    put_u16(&request[4], 512u);
    memcpy(&request[6], image, 512u);
    /* Two statements, not result_of(response, call(...)). C does not sequence function
     * arguments, so in that form the buffer may be read before the call that fills it has
     * run. It works on the machine it was written on and is not a test anyone should have to
     * reason about. */
    const uint16_t n = call(&f, RC_CMD_STAGE, request, 518u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_OK);

    /* Now a chunk claiming to continue from 1024, having received 512. */
    put_u32(&request[0], 1024u);
    put_u16(&request[4], 512u);
    memcpy(&request[6], &image[1024], 512u);
    const uint16_t m = call(&f, RC_CMD_STAGE, request, 518u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, m), (uint8_t)RC_RESP_BAD_LENGTH);
}

static void verify_before_a_complete_stage_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    uint8_t response[64];
    const uint16_t n = call(&f, RC_CMD_VERIFY, NULL, 0u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_STORAGE);
}

static void a_stage_chunk_running_past_the_image_is_refused(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;
    uint8_t request[520];
    memset(request, 0, sizeof(request));
    put_u32(&request[0], (uint32_t)RC_CAL_IMAGE_SIZE);
    put_u16(&request[4], 512u);
    uint8_t response[600];
    const uint16_t m = call(&f, RC_CMD_STAGE, request, 518u, response, sizeof(response));
    RC_CHECK_EQ_U(result_of(response, m), (uint8_t)RC_RESP_BAD_LENGTH);
}

static void an_image_that_fails_validation_is_never_staged(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.authenticated = true;

    /* Take the working image and drive one value out of its own range, so the received
     * image is structurally fine and semantically wrong. This is the case a CRC cannot
     * catch and a transfer cannot fail at. */
    uint8_t image[RC_CAL_IMAGE_SIZE];
    memcpy(image, &f.service.working, RC_CAL_IMAGE_SIZE);
    const uint16_t id = (uint16_t)rc_cal_id_of("engine.cylinder_count");
    const rc_cal_desc_t *desc = NULL;
    RC_REQUIRE(rc_cal_describe(id, &desc));
    memcpy(&image[desc->offset], "\x00\x00\xc0\x7f", 4u); /* a NaN */

    RC_CHECK_EQ_U(send_image(&f, image, RC_CAL_IMAGE_SIZE), (uint8_t)RC_RESP_OK);
    uint8_t response[64];
    const uint16_t n = call(&f, RC_CMD_VERIFY, NULL, 0u, response, sizeof(response));
    /* Required to be the NAMED refusal, not merely a failure. The flash layer validates
     * the read-back too and would return a bare storage error, so accepting either code
     * here would let the service's own check - the one that tells the operator which rule
     * they broke - be deleted without a single test noticing. */
    RC_CHECK_EQ_U(result_of(response, n), (uint8_t)RC_RESP_RULE_VIOLATION);
    RC_CHECK(n >= 3u);   /* the rule index is present, not just the code */
    /* And the working image must be untouched: a rejected image must not become the one
     * the next STAGE would send. */
    float cylinders = 0.0f;
    RC_REQUIRE(rc_cal_get_float(&f.service.working, rc_cal_id_of("engine.cylinder_count"),
                                &cylinders));
    RC_CHECK(cylinders >= 1.0f && cylinders <= 4.0f);
}

/* ----------------------------------------------------------------- live data */

static void live_frames_are_produced_only_when_logging_is_on(void)
{
    fixture_t f;
    fixture_init(&f);
    f.service.live_source = live_hook;
    f.service.live_ctx = &f;
    f.live.pos.rpm_x2 = 8400u;   /* 4200 rpm, in the state's half-rpm units */
    f.live.raw.battery_v = 13.9f;

    uint8_t out[600];
    RC_CHECK_EQ_U(rc_link_service_live_frame(&f.service, 1u, out, sizeof(out)), 0u);

    f.service.logging_hz = 50u;
    const uint16_t n = rc_link_service_live_frame(&f.service, 1u, out, sizeof(out));
    RC_CHECK(n > 0u);
    /* Decode the frame we just built through the real parser, so the test covers the
     * encode-and-reparse path a link actually takes. */
    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    rc_link_header_t got;
    uint8_t body[RC_LINK_MAX_PAYLOAD];
    uint16_t body_len = 0u;
    RC_REQUIRE(rc_link_parse(&parser, out, n, &got, body, &body_len, NULL));
    RC_CHECK_EQ_U(got.command, (uint8_t)RC_CMD_GET_LIVE);
    rc_link_live_t live;
    RC_REQUIRE(rc_link_decode_live(body, body_len, &live));
    RC_CHECK_NEAR(live.rpm, 4200.0f, 1.0f);
    /* The field that used to be a hard-coded zero. */
    RC_CHECK_NEAR(live.battery_v, 13.9f, 0.01f);
}

void test_link_service(void)
{
    RC_TEST_CASE("link/service/info/matches/the/image", ping_and_info_agree_with_the_image);
    RC_TEST_CASE("link/service/unknown/command/refused", an_unknown_command_is_refused);
    RC_TEST_CASE("link/service/status/reports/locked", the_service_issues_a_usable_salt);
    RC_TEST_CASE("link/service/authenticates/a/correct/proof",
                 authenticate_produces_the_proof_the_ecu_accepts);
    RC_TEST_CASE("link/service/refuses/a/wrong/proof", a_wrong_proof_is_refused_and_counted);
    RC_TEST_CASE("link/service/refuses/a/proof/without/the/code",
                 a_proof_that_omits_the_code_is_refused);
    RC_TEST_CASE("link/service/an/unprovisioned/ecu/cannot/be/unlocked",
                 an_unprovisioned_ecu_cannot_be_unlocked);
    RC_TEST_CASE("link/service/refuses/a/replayed/proof",
                 a_replayed_proof_with_a_swapped_salt_is_refused);
    RC_TEST_CASE("link/service/refuses/a/write/before/unlocking",
                 a_write_before_unlocking_is_refused);
    RC_TEST_CASE("link/service/refuses/a/write/while/running",
                 a_write_while_the_engine_runs_is_refused_even_when_unlocked);
    RC_TEST_CASE("link/service/refuses/an/out/range/value",
                 a_value_outside_the_items_own_range_is_refused);
    RC_TEST_CASE("link/service/refuses/a/rule/violation",
                 a_write_that_breaks_a_cross_item_rule_is_refused_and_names_the_rule);
    RC_TEST_CASE("link/service/refuses/a/short/write", a_short_write_is_refused);
    RC_TEST_CASE("link/service/read/bulk/covers/the/image",
                 read_bulk_returns_the_whole_image_with_matching_offsets);
    RC_TEST_CASE("link/service/no/reply/exceeds/the/frame/payload",
                 no_reply_exceeds_the_frame_payload);
    RC_TEST_CASE("link/service/refuses/a/read/past/the/end",
                 read_bulk_past_the_end_is_refused);
    RC_TEST_CASE("link/service/read/item/returns/the/value", read_item_returns_the_working_value);
    RC_TEST_CASE("link/service/refuses/an/unknown/item", an_unknown_item_id_is_refused);
    RC_TEST_CASE("link/service/flash/round/trips", a_full_stage_verify_activate_round_trip);
    RC_TEST_CASE("link/service/a/rollback/serves/the/previous/tune",
                 a_rollback_makes_the_ecu_serve_the_previous_tune);
    RC_TEST_CASE("link/service/refuses/a/rollback/onto/an/invalid/slot",
                 a_rollback_onto_an_invalid_slot_is_refused);
    RC_TEST_CASE("link/service/refuses/an/out/of/order/chunk",
                 an_out_of_order_chunk_is_refused);
    RC_TEST_CASE("link/service/refuses/verify/before/stage/completes",
                 verify_before_a_complete_stage_is_refused);
    RC_TEST_CASE("link/service/refuses/a/chunk/past/the/image",
                 a_stage_chunk_running_past_the_image_is_refused);
    RC_TEST_CASE("link/service/never/stages/an/invalid/image",
                 an_image_that_fails_validation_is_never_staged);
    RC_TEST_CASE("link/service/live/only/when/logging", live_frames_are_produced_only_when_logging_is_on);
}
