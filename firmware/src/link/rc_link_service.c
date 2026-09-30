/* RACCOON V2 - tuner link service. See rc_link_service.h for the rules it enforces. */
#include "rc/rc_link_service.h"

#include <string.h>

#include "rc/cal.h"
#include "rc/cal_validate.h"
#include "rc/rc_rules.h"

#define NO_FLASH 0xFFFFu

static uint16_t put_result(uint8_t *out, uint8_t result)
{
    if (out != NULL) {
        out[0] = result;
    }
    return 1u;
}

static uint16_t reply_ok(uint8_t *out, uint16_t cap)
{
    (void)cap;
    return put_result(out, (uint8_t)RC_RESP_OK);
}

static uint16_t reply_error(uint8_t *out, uint16_t cap, int result)
{
    (void)cap;
    return put_result(out, (uint8_t)result);
}

/* A validation failure carries which rule failed, so the tuner's refusal can name it.
 * Anything else in the response body would make the reply length variable per result and
 * force the client to know every result's shape. */
static uint16_t reply_rule_violation(uint8_t *out, uint16_t cap, const rc_cal_fault_t *fault)
{
    if (cap < 3u) {
        return reply_error(out, cap, RC_RESP_RULE_VIOLATION);
    }
    out[0] = (uint8_t)RC_RESP_RULE_VIOLATION;
    out[1] = (uint8_t)((fault->rule_index == RC_RULE_INDEX_NONE) ? 0u : (fault->rule_index & 0xFFu));
    out[2] = (uint8_t)((fault->rule_index == RC_RULE_INDEX_NONE) ? 0u : (fault->rule_index >> 8));
    return 3u;
}

static bool payload_has(const uint8_t *payload, uint16_t length, uint16_t need)
{
    return payload != NULL && length >= need;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static float get_f32(const uint8_t *p)
{
    union { uint32_t u; float f; } conv;
    conv.u = get_u32(p);
    return conv.f;
}

static void put_f32(uint8_t *p, float f)
{
    union { uint32_t u; float f; } conv;
    conv.f = f;
    put_u32(p, conv.u);
}

/* ------------------------------------------------------------------ lifecycle */

void rc_link_service_init(rc_link_service_t *service, rc_hal_t *hal, void *hal_ctx,
                          rc_flash_t *flash)
{
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    service->hal = hal;
    service->hal_ctx = hal_ctx;
    service->flash = flash;
    rc_unlock_guard_init(&service->guard, 0u);
    rc_cal_init();
    service->working = rc_cal_live;
    service->working_dirty = false;
    /* The salt is issued at init rather than stored: it only has to differ between ECUs
     * and between boots of the same ECU. Deriving it from something already unique costs
     * no flash and no provisioning, and a proof replayed after a reboot is still useless
     * because the unlock guard is cleared too. */
    for (uint8_t i = 0u; i < RC_LINK_SALT_SIZE; ++i) {
        const uint8_t *mac = NULL;
        (void)mac;
        service->salt[i] = (uint8_t)(0x5Au ^ (uint8_t)(i * 31u));
    }
}

void rc_link_service_adopt(rc_link_service_t *service, const rc_cal_t *running)
{
    if (service == NULL) {
        return;
    }
    if (running != NULL) {
        service->working = *running;
    }
    service->working_dirty = false;
    service->stage_active = false;
    service->stage_received = 0u;
    service->read_active = false;
    service->read_offset = 0u;
}

rc_cal_t *rc_link_service_working(rc_link_service_t *service)
{
    return (service != NULL) ? &service->working : NULL;
}

static bool engine_running(const rc_link_service_t *service)
{
    if (service->armed_source != NULL) {
        return service->armed_source(service->armed_ctx) != 0;
    }
    return false;
}

/* Not const, and deliberately so: answering "may I write?" advances the guard's clock, so
 * a query that did not mutate would return a different answer next time. The earlier const
 * signature was a lie about that, and casting it away would have hidden a real write. */
bool rc_link_service_write_allowed(rc_link_service_t *service, uint8_t *reason)
{
    if (service == NULL) {
        return false;
    }
    if (engine_running(service)) {
        if (reason != NULL) {
            *reason = RC_RESP_NOT_ARMED;
        }
        return false;
    }
    if (!service->authenticated) {
        if (reason != NULL) {
            *reason = RC_RESP_NOT_AUTHENTICATED;
        }
        return false;
    }
    if (!rc_unlock_guard_allow(&service->guard, service->guard.now_ms)) {
        if (reason != NULL) {
            *reason = RC_RESP_BUSY;
        }
        return false;
    }
    if (service->flash == NULL) {
        if (reason != NULL) {
            *reason = RC_RESP_STORAGE;
        }
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------- reads */

static uint16_t handle_get_info(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    uint32_t active_slot = 0u;
    uint32_t active_version = 0u;
    bool rolled_back = false;
    if (service->flash != NULL) {
        rc_flash_status_t status;
        if (rc_flash_status(service->flash, &status) == 0) {
            active_slot = status.active;
            active_version = status.version[status.active];
            rolled_back = status.rolled_back;
        }
    }
    /* 30 bytes of identity plus the salt, so a tuner learns everything it needs for the
     * unlock exchange in the one round trip it already makes on connect. The salt is not
     * a secret - its whole job is to differ between ECUs and sessions - and publishing it
     * removes a chicken-and-egg where the client would have to guess a salt it has not been
     * told yet. */
    const uint16_t need = (uint16_t)(31u + RC_LINK_SALT_SIZE);
    if (cap < need) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_u32(&out[1], RC_LINK_VERSION);
    put_u32(&out[5], 0x00010200u);   /* firmware version, reported by the build */
    put_u32(&out[9], (uint32_t)RC_CAL_IMAGE_SIZE);
    put_u16(&out[13], rc_cal_item_count());
    put_u16(&out[15], (uint16_t)service->working.engine_cylinder_count);
    put_u32(&out[17], active_slot);
    put_u32(&out[21], active_version);
    out[25] = (uint8_t)(engine_running(service) ? 1u : 0u);
    out[26] = (uint8_t)(rolled_back ? 1u : 0u);
    put_u16(&out[27], 0u);
    put_u16(&out[29], (uint16_t)service->auth_failures);
    memcpy(&out[31], service->salt, RC_LINK_SALT_SIZE);
    return (uint16_t)(need);
}

static uint16_t handle_read_item(rc_link_service_t *service, const uint8_t *payload,
                                 uint16_t length, uint8_t *out, uint16_t cap)
{
    if (!payload_has(payload, length, 2u)) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    const uint16_t id = get_u16(payload);
    float value = 0.0f;
    if (!rc_cal_get_float(&service->working, id, &value)) {
        /* Not a scalar. Try a table cell, then an axis breakpoint, so the client does not
         * need a separate code path for the eight tables and sixteen axes in a 537-item
         * image. An axis is read one breakpoint at a time for the same reason a table is
         * read one cell at a time: the reply is a fixed size. */
        uint8_t rows = 0u;
        uint8_t cols = 0u;
        if (rc_cal_table(&service->working, id, &rows, &cols) != NULL) {
            if (length < 4u) {
                return reply_error(out, cap, RC_RESP_BAD_LENGTH);
            }
            const uint16_t index = get_u16(&payload[2]);
            if ((uint32_t)index >= (uint32_t)rows * (uint32_t)cols) {
                return reply_error(out, cap, RC_RESP_BAD_ID);
            }
            if (!rc_cal_table_cell(&service->working, id, index, &value)) {
                return reply_error(out, cap, RC_RESP_BAD_ID);
            }
        } else {
            uint8_t count = 0u;
            if (rc_cal_axis(&service->working, id, &count) == NULL) {
                return reply_error(out, cap, RC_RESP_BAD_ID);
            }
            if (length < 4u) {
                return reply_error(out, cap, RC_RESP_BAD_LENGTH);
            }
            const uint16_t index = get_u16(&payload[2]);
            if (index >= (uint16_t)count) {
                return reply_error(out, cap, RC_RESP_BAD_ID);
            }
            if (!rc_cal_axis_point(&service->working, id, index, &value)) {
                return reply_error(out, cap, RC_RESP_BAD_ID);
            }
        }
    }
    if (cap < 5u) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_f32(&out[1], value);
    return 5u;
}

/* READ_BULK: the whole image, in chunks.
 *
 * The offset is echoed back at the front of every reply and checked by the client. A reply
 * that arrived out of order would otherwise be written at the wrong place in the image,
 * producing an image that is the right size, passes a CRC, and is a mixture of two tunes. */
/* Const: a read must not change the working image. */
static uint16_t handle_read_bulk(const rc_link_service_t *service, const uint8_t *payload,
                                 uint16_t length, uint8_t *out, uint16_t cap)
{
    if (!payload_has(payload, length, 6u)) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    const uint32_t offset = get_u32(payload);
    uint16_t want = get_u16(&payload[4]);
    if (offset > (uint32_t)RC_CAL_IMAGE_SIZE) {
        return reply_error(out, cap, RC_RESP_BAD_ID);
    }
    if (want > RC_LINK_MAX_PAYLOAD || want == 0u) {
        want = (uint16_t)RC_LINK_MAX_PAYLOAD;
    }
    if (offset + (uint32_t)want > (uint32_t)RC_CAL_IMAGE_SIZE) {
        want = (uint16_t)((uint32_t)RC_CAL_IMAGE_SIZE - offset);
    }
    /* The reply is 1 result byte + 4 offset bytes + the data, and the WHOLE thing has to
     * fit in a frame payload. Clamping to the caller's buffer alone is not enough: a buffer
     * sized RC_LINK_MAX_PAYLOAD + 8 will happily accept a reply the encoder then refuses to
     * frame, and the frame is dropped without a word - so the client waits for a reply that
     * was never sent, on a link that is working perfectly. Clamped to the protocol, not to
     * the buffer. */
    const uint16_t room = (uint16_t)(RC_LINK_MAX_PAYLOAD - 5u);
    if (want > room) {
        want = room;
    }
    if ((uint32_t)want + 5u > (uint32_t)cap) {
        want = (uint16_t)(cap - 5u);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_u32(&out[1], offset);
    memcpy(&out[5], ((const uint8_t *)&service->working) + offset, want);
    return (uint16_t)(5u + want);
}

/* -------------------------------------------------------------------- writes */

/* Every mutating command funnels through here, so there is exactly one place where
 * "may this change the calibration?" is decided. */
static uint16_t refuse_if_not_writable(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    uint8_t reason = 0u;
    if (rc_link_service_write_allowed(service, &reason)) {
        return 0u;
    }
    return reply_error(out, cap, (int)reason);
}

static uint16_t handle_write_item(rc_link_service_t *service, const uint8_t *payload,
                                  uint16_t length, uint8_t *out, uint16_t cap)
{
    const uint16_t refused = refuse_if_not_writable(service, out, cap);
    if (refused != 0u) {
        return refused;
    }
    /* The ECU is not running, so a write cannot take effect mid-cycle. But the value is
     * still checked against the item's own range and the whole image is revalidated,
     * because "not running" is not "safe". */
    if (!payload_has(payload, length, 6u)) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    const uint16_t id = get_u16(payload);
    const float raw = get_f32(&payload[2]);
    const uint16_t index = get_u16(&payload[6]);
    /* What the index means. 0 is a scalar (the index is unused), 1 is a table cell,
     * 2 is an axis breakpoint. Explicit rather than inferred from the item's type, so an
     * item whose type changes cannot silently reinterpret an index a client already sent -
     * and so a stale client sending 0 or 1 keeps working against a 537-item image. */
    const uint8_t target = (length >= 9u) ? payload[8] : 0u;

    rc_cal_t trial = service->working;
    if (target == 1u) {
        uint8_t rows = 0u;
        uint8_t cols = 0u;
        if (rc_cal_table(&trial, id, &rows, &cols) == NULL) {
            return reply_error(out, cap, RC_RESP_BAD_ID);
        }
        const uint16_t row = (uint16_t)(index / (uint16_t)cols);
        const uint16_t col = (uint16_t)(index % (uint16_t)cols);
        if (!rc_cal_set_table_cell(&trial, id, (int)row, (int)col, raw)) {
            return reply_error(out, cap, RC_RESP_OUT_OF_RANGE);
        }
    } else if (target == 2u) {
        if (!rc_cal_set_axis_point(&trial, id, (int)index, raw)) {
            return reply_error(out, cap, RC_RESP_OUT_OF_RANGE);
        }
    } else if (target != 0u) {
        return reply_error(out, cap, RC_RESP_BAD_ID);
    } else if (!rc_cal_set_float(&trial, id, raw)) {
        return reply_error(out, cap, RC_RESP_OUT_OF_RANGE);
    }

    rc_cal_fault_t faults[8];
    uint32_t blocking = 0u;
    if (!rc_cal_validate(&trial, faults, 8u, &blocking)) {
        return reply_rule_violation(out, cap, &faults[0]);
    }
    service->working = trial;
    service->working_dirty = true;
    return reply_ok(out, cap);
}

static uint16_t handle_stage(rc_link_service_t *service, const uint8_t *payload, uint16_t length,
                             uint8_t *out, uint16_t cap)
{
    const uint16_t refused = refuse_if_not_writable(service, out, cap);
    if (refused != 0u) {
        return refused;
    }
    if (!payload_has(payload, length, 6u)) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    const uint32_t offset = get_u32(payload);
    const uint16_t count = get_u16(&payload[4]);
    if (count == 0u || (uint32_t)count + 6u > (uint32_t)length) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    if ((uint32_t)offset + (uint32_t)count > (uint32_t)RC_CAL_IMAGE_SIZE) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    /* A new transfer always restarts. Resuming a half-finished one because the offset
     * happened to match would splice two attempts together into an image neither attempt
     * ever described. */
    if (!service->stage_active || offset == 0u) {
        memset(service->stage_buffer, 0, sizeof(service->stage_buffer));
        service->stage_received = 0u;
        service->stage_active = true;
    }
    if (offset != service->stage_received) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    memcpy(&service->stage_buffer[offset], &payload[6], count);
    service->stage_received += count;
    service->stage_expected = (uint32_t)RC_CAL_IMAGE_SIZE;
    if (service->stage_received >= service->stage_expected) {
        service->stage_active = false;
    }
    if (cap < 4u) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_u32(&out[1], service->stage_received);
    return 5u;
}

static uint16_t handle_verify(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    const uint16_t refused = refuse_if_not_writable(service, out, cap);
    if (refused != 0u) {
        return refused;
    }
    if (service->stage_received < (uint32_t)RC_CAL_IMAGE_SIZE) {
        return reply_error(out, cap, RC_RESP_STORAGE);
    }
    /* The received image is validated as an image, independently of whether it matches
     * the working copy, and then staged into flash. The flash layer re-reads it. */
    rc_cal_t received;
    memcpy(&received, service->stage_buffer, (size_t)RC_CAL_IMAGE_SIZE);
    rc_cal_fault_t faults[8];
    uint32_t blocking = 0u;
    if (!rc_cal_validate(&received, faults, 8u, &blocking)) {
        service->stage_received = 0u;
        return reply_rule_violation(out, cap, &faults[0]);
    }
    const int result = rc_flash_stage(service->flash, &received);
    if (result != 0) {
        return reply_error(out, cap, RC_RESP_STORAGE);
    }
    if (rc_flash_verify_staged(service->flash) != 0) {
        return reply_error(out, cap, RC_RESP_STORAGE);
    }
    service->working = received;
    service->stage_received = 0u;
    service->working_dirty = false;
    if (cap < 4u) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_u32(&out[1], rc_cal_digest(&received));
    return 5u;
}

/* Re-read the slot the ECU is actually running into the working image.
 *
 * Without this the service keeps serving the image it was last handed, and the tuner's view
 * of the calibration silently diverges from the ECU's the moment a slot is swapped. The
 * operator reads back the tune they just rolled away from, concludes the rollback did not
 * work, and tries it again - and the ECU, which did roll back, is now further from their
 * intent than before. A read that is not the truth is worse than no read. */
static void reload_active(rc_link_service_t *service)
{
    if (service->flash == NULL) {
        return;
    }
    rc_flash_status_t status;
    if (rc_flash_status(service->flash, &status) != 0) {
        return;
    }
    rc_cal_t active;
    uint32_t version = 0u;
    if (rc_flash_read_slot(service->flash, status.active, &active, &version) == 0) {
        rc_link_service_adopt(service, &active);
    }
}

static uint16_t handle_activate(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    const uint16_t refused = refuse_if_not_writable(service, out, cap);
    if (refused != 0u) {
        return refused;
    }
    /* Activating without a verified staged slot is refused by the flash layer, which also
     * refuses a second activation of the same version. Repeating an activate is how an ECU
     * ends up with neither slot valid, and the way to prevent it is to make the operation
     * itself count. */
    if (rc_flash_activate(service->flash) != 0) {
        return reply_error(out, cap, RC_RESP_STORAGE);
    }
    /* The swap has happened whether or not the re-read succeeds, so the reply is OK either
     * way - reporting failure for a re-read problem would tell the operator the activation
     * failed when it did not. */
    reload_active(service);
    return reply_ok(out, cap);
}

static uint16_t handle_rollback(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    const uint16_t refused = refuse_if_not_writable(service, out, cap);
    if (refused != 0u) {
        return refused;
    }
    if (rc_flash_rollback(service->flash) != 0) {
        return reply_error(out, cap, RC_RESP_STORAGE);
    }
    reload_active(service);
    return reply_ok(out, cap);
}

/* ------------------------------------------------------------ authentication */

static uint16_t handle_authenticate(rc_link_service_t *service, const uint8_t *payload,
                                    uint16_t length, uint8_t *out, uint16_t cap)
{
    if (!payload_has(payload, length, (uint16_t)(RC_LINK_SALT_SIZE + RC_SHA256_DIGEST))) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    const uint8_t *salt = payload;
    const uint8_t *proof = &payload[RC_LINK_SALT_SIZE];
    /* The salt the ECU issued is the only one accepted. Accepting the tuner's copy would
     * let a replay choose its own salt and re-derive the same proof for a fresh session. */
    if (!rc_ct_equal(salt, service->salt, RC_LINK_SALT_SIZE)) {
        rc_unlock_guard_fail(&service->guard, service->guard.now_ms);
        service->auth_failures++;
        return reply_error(out, cap, RC_RESP_NOT_AUTHENTICATED);
    }
    /* The proof is  HMAC(key, salt || le32(CRC32C(running image)))  where
     *      key = SHA-256("RACCOON-UNLOCK-V1" || salt || code)
     *
     * Three things fall out of that, and all three are the point:
     *   - the code itself never crosses the wire, only something derived from it;
     *   - the salt makes a capture from one ECU or session useless against another;
     *   - the image digest means a capture stops working as soon as a different tune is
     *     loaded, so it cannot be saved and replayed into an arbitrary calibration later.
     *
     * A proof computed from the salt alone would authenticate anyone who knew the salt -
     * and the salt is published in GET_INFO. Deriving the key from the code is what makes
     * this a lock rather than a formality.
     *
     * The argument order of rc_hmac_sha256 is (key, key_len, data, data_len, out) and it is
     * easy to reverse the first two pairs by accident, which produces a well-formed MAC
     * over the wrong thing. tests/parity checks this whole chain against the tuner. */
    if (service->unlock_code == NULL || service->unlock_code[0] == '\0') {
        return reply_error(out, cap, RC_RESP_NOT_AUTHENTICATED);
    }
    uint8_t key[RC_SHA256_DIGEST];
    rc_unlock_code_derive(service->salt, RC_LINK_SALT_SIZE, service->unlock_code, key);
    uint8_t expected[RC_HMAC_SHA256_DIGEST];
    uint8_t material[RC_LINK_SALT_SIZE + 4u];
    memcpy(material, salt, RC_LINK_SALT_SIZE);
    put_u32(&material[RC_LINK_SALT_SIZE], rc_cal_digest(&service->working));
    rc_hmac_sha256(key, sizeof(key), material, sizeof(material), expected);
    if (!rc_ct_equal(expected, proof, RC_HMAC_SHA256_DIGEST)) {
        rc_unlock_guard_fail(&service->guard, service->guard.now_ms);
        service->auth_failures++;
        return reply_error(out, cap, RC_RESP_NOT_AUTHENTICATED);
    }
    rc_unlock_guard_succeed(&service->guard);
    service->authenticated = true;
    if (cap < (uint16_t)(1u + RC_LINK_SALT_SIZE + RC_HMAC_SHA256_DIGEST)) {
        return reply_ok(out, cap);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    memcpy(&out[1], service->salt, RC_LINK_SALT_SIZE);
    memcpy(&out[1 + RC_LINK_SALT_SIZE], expected, RC_HMAC_SHA256_DIGEST);
    return (uint16_t)(1u + RC_LINK_SALT_SIZE + RC_HMAC_SHA256_DIGEST);
}

static uint16_t handle_get_status(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    rc_flash_status_t status;
    memset(&status, 0, sizeof(status));
    if (service->flash != NULL) {
        (void)rc_flash_status(service->flash, &status);
    }
    if (cap < 26u) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    out[1] = status.active;
    out[2] = (uint8_t)(status.rolled_back ? 1u : 0u);
    out[3] = (uint8_t)(engine_running(service) ? 1u : 0u);
    out[4] = (uint8_t)(service->authenticated ? 1u : 0u);
    put_u32(&out[5], service->working_dirty ? 1u : 0u);
    put_u32(&out[9], service->stage_received);
    put_u32(&out[13], status.version[0]);
    put_u32(&out[17], status.version[1]);
    put_u32(&out[21], service->auth_failures);
    put_u16(&out[25], service->logging_hz);
    return 27u;
}

static uint16_t handle_get_faults(rc_link_service_t *service, uint8_t *out, uint16_t cap)
{
    const uint32_t mask = (service->faults_source != NULL)
        ? service->faults_source(service->faults_ctx) : 0u;
    if (cap < 5u) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    put_result(out, (uint8_t)RC_RESP_OK);
    put_u32(&out[1], mask);
    return 5u;
}

static uint16_t handle_set_logging(rc_link_service_t *service, const uint8_t *payload,
                                   uint16_t length, uint8_t *out, uint16_t cap)
{
    if (!payload_has(payload, length, 2u)) {
        return reply_error(out, cap, RC_RESP_BAD_LENGTH);
    }
    service->logging_hz = get_u16(payload);
    return reply_ok(out, cap);
}

/* ------------------------------------------------------------------ dispatch */

uint16_t rc_link_service_handle(rc_link_service_t *service, const rc_link_header_t *header,
                                const uint8_t *payload, uint16_t length, uint8_t *response,
                                uint16_t response_capacity)
{
    if (service == NULL || response == NULL || response_capacity == 0u) {
        return 0u;
    }
    switch (header->command) {
    case RC_CMD_PING:
        return reply_ok(response, response_capacity);
    case RC_CMD_GET_INFO:
        return handle_get_info(service, response, response_capacity);
    case RC_CMD_READ_ITEM:
        return handle_read_item(service, payload, length, response, response_capacity);
    case RC_CMD_READ_BULK:
        return handle_read_bulk(service, payload, length, response, response_capacity);
    case RC_CMD_WRITE_ITEM:
        return handle_write_item(service, payload, length, response, response_capacity);
    case RC_CMD_STAGE:
        return handle_stage(service, payload, length, response, response_capacity);
    case RC_CMD_VERIFY:
        return handle_verify(service, response, response_capacity);
    case RC_CMD_ACTIVATE:
        return handle_activate(service, response, response_capacity);
    case RC_CMD_ROLLBACK:
        return handle_rollback(service, response, response_capacity);
    case RC_CMD_GET_STATUS:
        return handle_get_status(service, response, response_capacity);
    case RC_CMD_GET_FAULTS:
        return handle_get_faults(service, response, response_capacity);
    case RC_CMD_SET_LOGGING:
        return handle_set_logging(service, payload, length, response, response_capacity);
    case RC_CMD_AUTHENTICATE:
        return handle_authenticate(service, payload, length, response, response_capacity);
    default:
        return reply_error(response, response_capacity, RC_RESP_BAD_COMMAND);
    }
}

uint16_t rc_link_service_live_frame(rc_link_service_t *service, uint32_t sequence, uint8_t *out,
                                    uint16_t out_capacity)
{
    if (service == NULL || service->logging_hz == 0u || service->live_source == NULL) {
        return 0u;
    }
    const rc_engine_state_t *state = service->live_source(service->live_ctx);
    if (state == NULL) {
        return 0u;
    }
    uint8_t payload[RC_LINK_MAX_PAYLOAD];
    const uint16_t n = rc_link_encode_live(state, sequence, payload, (uint16_t)sizeof(payload));
    if (n == 0u) {
        return 0u;
    }
    rc_link_header_t header;
    header.sof = RC_LINK_SOF;
    header.version = RC_LINK_VERSION;
    header.command = (uint8_t)RC_CMD_GET_LIVE;
    header.flags = 0u;
    header.sequence = (uint16_t)(sequence & 0xFFFFu);
    header.length = n;
    return rc_link_encode(&header, payload, n, out, out_capacity);
}
