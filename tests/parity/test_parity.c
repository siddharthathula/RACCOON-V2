/* RACCOON V2 - wire-format parity check.
 *
 * The tuner and the ECU implement the same protocol in two languages. Nothing about that
 * is safe by construction, and the failure is silent and dangerous: a length field read at
 * a different offset turns ACTIVATE into something else, and the ECU does what it is told.
 *
 * So the C encoder/decoder and the Python one are run over the SAME inputs here, and their
 * bytes are compared. If either side is changed and the other is not, this fails.
 */
#include "rc_test.h"

#include "cal_types.h"
#include "rc/cal.h"
#include "rc/rc_crc32.h"
#include "rc/rc_crypto.h"
#include "rc/rc_link.h"

/* The Python reference, executed at build time by tools/gen_parity.py. */
#include "parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Python reference, generated into C by the test harness so it cannot drift. */
extern const char *rc_parity_python_frame(const char *hex_payload);

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t max_len)
{
    const size_t len = strlen(hex) / 2u;
    if (len > max_len) {
        return false;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned value = 0u;
        if (sscanf(&hex[i * 2u], "%2x", &value) != 1) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}


/** The two encoders must produce identical bytes for the same request. */
static void encoders_agree_byte_for_byte(void)
{
    static const char *payloads[] = {
        "",
        "00",
        "0102030405060708",
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    };
    for (size_t p = 0u; p < sizeof(payloads) / sizeof(payloads[0]); ++p) {
        uint8_t payload[64];
        const size_t len = strlen(payloads[p]) / 2u;
        RC_REQUIRE(hex_to_bytes(payloads[p], payload, sizeof(payload)));

        rc_link_header_t header;
        memset(&header, 0, sizeof(header));
        header.command = RC_CMD_WRITE_ITEM;
        header.flags = 0x00u;
        header.sequence = 0x1234u;
        uint8_t c_frame[RC_LINK_MAX_FRAME];
        const uint16_t c_len = rc_link_encode(&header, (len > 0u) ? payload : NULL,
                                              (uint16_t)len, c_frame, (uint16_t)sizeof(c_frame));
        RC_CHECK(c_len > 0u);

        char c_hex[RC_LINK_MAX_FRAME * 2u + 1u];
        for (uint16_t i = 0u; i < c_len; ++i) {
            (void)snprintf(&c_hex[i * 2u], 3u, "%02x", (unsigned)c_frame[i]);
        }
        c_hex[c_len * 2u] = '\0';

        const char *python_hex = rc_parity_python_frame(payloads[p]);
        RC_CHECK_MSG(python_hex != NULL && python_hex[0] != '\0',
                     "no Python reference frame for payload %u", (unsigned)p);
        if (python_hex == NULL || python_hex[0] == '\0') {
            return;
        }
        RC_CHECK_MSG(strcmp(c_hex, python_hex) == 0,
                     "payload %u: C produced %s, Python produced %s", (unsigned)p, c_hex,
                     python_hex);
    }
}

/** And the two decoders must agree about the same bytes, including rejecting the same
 *  corrupt ones. */
static void decoders_agree_on_good_and_corrupt_frames(void)
{
    uint8_t payload[16];
    for (uint8_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 11u);
    }
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_READ_BULK;
    header.sequence = 0x4321u;
    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t len = rc_link_encode(&header, payload, (uint16_t)sizeof(payload), frame,
                                        (uint16_t)sizeof(frame));

    char hex[RC_LINK_MAX_FRAME * 2u + 1u];
    for (uint16_t i = 0u; i < len; ++i) {
        (void)snprintf(&hex[i * 2u], 3u, "%02x", (unsigned)frame[i]);
    }
    hex[len * 2u] = '\0';

    /* The Python decoder must accept it, and report the same fields. */
    const int py_ok = rc_parity_python_decode_check(hex);
    RC_CHECK_MSG(py_ok == 1, "the Python decoder rejected a frame the C encoder produced");

    /* Corrupt one bit: both must reject. */
    frame[10] ^= 0x08u;
    char bad_hex[RC_LINK_MAX_FRAME * 2u + 1u];
    for (uint16_t i = 0u; i < len; ++i) {
        (void)snprintf(&bad_hex[i * 2u], 3u, "%02x", (unsigned)frame[i]);
    }
    bad_hex[len * 2u] = '\0';
    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    rc_link_header_t got;
    RC_CHECK(rc_link_parse(&parser, frame, len, &got, NULL, NULL, NULL) == false);
    RC_CHECK_MSG(rc_parity_python_decode_check(bad_hex) == 0,
                 "the Python decoder accepted a frame the C decoder rejected");
}

/** The live-data layout is the most dangerous to get wrong, because a field in the wrong
 *  place still decodes: it just decodes as something else. */
static void live_data_layouts_agree(void)
{
    rc_engine_state_t state;
    memset(&state, 0, sizeof(state));
    state.engine_time_us = 4242u;
    state.pos.rpm_x2 = 6500u;
    state.load = 55.25f;
    state.load_source = (uint8_t)RC_LOAD_MAP;
    state.lambda[0] = 0.93f;
    state.lambda[1] = 0.94f;
    state.lambda_valid[0] = 1u;
    state.pos.sync_state = (uint8_t)RC_TRIGGER_SYNCED;
    state.pos.sync_confidence = 100u;
    state.ignition_deg[0] = 11.5f;
    state.ignition_deg[3] = 12.5f;
    state.injector_ms[0] = 3.25f;
    state.dwell_ms[1] = 3.5f;
    /* Every field the frame carries is set here, including the raw sensor readings. A
     * parity test that leaves a field at zero still passes if both sides happen to agree
     * about where zero lives, and a field both sides forgot entirely is exactly the drift
     * this is meant to catch. */
    state.raw.tps_pct = 23.5f;
    state.raw.iat_c = 31.0f;
    state.raw.ect_c = 88.5f;
    state.raw.battery_v = 13.8f;
    state.raw.fuel_pressure_kpa = 310.0f;
    state.raw.oil_pressure_bar = 4.2f;
    state.raw.oil_temp_c = 96.0f;
    state.ignition_cut[2] = 1u;
    state.safety_active = 0x0400u;
    state.corr.torque_allowance = 0.65f;
    state.corr.safety_factor = 0.9f;
    state.armed = 1u;

    uint8_t payload[512];
    const uint16_t len = rc_link_encode_live(&state, 777u, payload, (uint16_t)sizeof(payload));
    RC_REQUIRE(len > 0u);

    char hex[1024];
    for (uint16_t i = 0u; i < len; ++i) {
        (void)snprintf(&hex[i * 2u], 3u, "%02x", (unsigned)payload[i]);
    }
    hex[len * 2u] = '\0';

    /* Returns bit 0 = sizes agree, and the Python decode must produce the same numbers. */
    const int py = rc_parity_live_check(hex);
    RC_CHECK_MSG(py == 1,
                 "the C live-data layout does not match the Python one (lookup returned %d); "
                 "the encoders have drifted apart", py);
}

/** The calibration image the tuner will write must be byte-identical to the one the
 *  firmware runs.
 *
 *  This is the single most important check in the project after the safety tests. A tuner
 *  image that is the right SIZE and wrong in its bytes is accepted by the GUI, written
 *  without complaint, passes the tuner's own validation because the tuner has the same
 *  wrong idea, and is then rejected by the ECU's CRC - or worse, not rejected, if the
 *  difference happens to land in padding. */
static void the_tuner_image_matches_the_firmware_image(void)
{
    extern const char *tuner_default_image_hex(void);
    rc_cal_init();
    const char *c_hex = rc_parity_c_default_image();
    const char *py_hex = tuner_default_image_hex();
    RC_REQUIRE(c_hex != NULL && py_hex != NULL);
    RC_CHECK_MSG(strcmp(c_hex, py_hex) == 0,
                 "the tuner's default calibration image differs from the firmware's\n"
                 "  firmware: %.96s...\n  tuner:   %.96s...",
                 c_hex, py_hex);
}

/** The unlock proof depends on the firmware's own CRC, and the firmware's CRC is CRC-32C -
 *  not the CRC-32 that zlib, hashlib and most people's memory all supply.
 *
 *  A mismatch here does not fail loudly. The client computes a perfectly well-formed proof
 *  over the wrong polynomial, sends it, and the ECU refuses every unlock attempt with
 *  NOT_AUTHENTICATED and nothing else. The operator's conclusion is that the code is
 *  wrong. This check is the difference between that and a one-line fix.
 */
static void the_tuners_crc_and_unlock_proof_match_the_firmware(void)
{
    extern const char *tuner_crc32c_probe(void);
    extern const char *tuner_unlock_proof(void);

    char expected[32];
    (void)snprintf(expected, sizeof(expected), "%08x%08x", (unsigned)rc_crc32c(0u, "", 0u),
                   (unsigned)rc_crc32c(0u, "abc", 3u));
    RC_CHECK_MSG(strcmp(expected, tuner_crc32c_probe()) == 0,
                 "the tuner and the firmware disagree about CRC-32C: firmware %s, tuner %s",
                 expected, tuner_crc32c_probe());

    /* And the whole proof, which is the thing an operator actually experiences failing. */
    uint8_t salt[16];
    for (uint8_t i = 0u; i < 16u; ++i) {
        salt[i] = i;
    }
    /* rc_cal_live is filled by rc_cal_init(), which nothing else in this file calls. Read
     * before initialising it, the image is whatever the linker left behind: the comparison
     * then fails for a reason that has nothing to do with the CRC, which is the most
     * expensive kind of failure to debug. */
    rc_cal_init();
    const rc_cal_t *image = &rc_cal_live;
    RC_REQUIRE(rc_cal_digest(image) != 0u);
    uint8_t material[20];
    memcpy(material, salt, 16u);
    const uint32_t digest = rc_crc32c(0u, (const void *)image, (size_t)sizeof(*image));
    material[16] = (uint8_t)(digest & 0xFFu);
    material[17] = (uint8_t)((digest >> 8) & 0xFFu);
    material[18] = (uint8_t)((digest >> 16) & 0xFFu);
    material[19] = (uint8_t)((digest >> 24) & 0xFFu);
    uint8_t key[RC_SHA256_DIGEST];
    rc_unlock_code_derive(salt, 16u, "parity-check-not-a-secret", key);
    uint8_t proof[16 + RC_SHA256_DIGEST];
    memcpy(proof, salt, 16u);
    rc_hmac_sha256(key, sizeof(key), material, sizeof(material), &proof[16]);

    char firmware_hex[128];
    for (unsigned i = 0u; i < sizeof(proof); ++i) {
        (void)snprintf(&firmware_hex[i * 2u], 3u, "%02x", (unsigned)proof[i]);
    }
    firmware_hex[sizeof(proof) * 2u] = '\0';
    RC_CHECK_MSG(strcmp(firmware_hex, tuner_unlock_proof()) == 0,
                 "the tuner's unlock proof does not match the firmware's\n"
                 "  firmware: %.96s\n  tuner:   %.96s", firmware_hex, tuner_unlock_proof());
}

void test_parity(void)
{
    RC_TEST_CASE("encoders/agree/byte/for/byte", encoders_agree_byte_for_byte);
    RC_TEST_CASE("decoders/agree/on/good/and/corrupt/frames",
                 decoders_agree_on_good_and_corrupt_frames);
    RC_TEST_CASE("live/data/layouts/agree", live_data_layouts_agree);
    RC_TEST_CASE("the/tuner/image/matches/the/firmware/image",
                 the_tuner_image_matches_the_firmware_image);
    RC_TEST_CASE("the/tuners/crc/and/unlock/proof/match/the/firmware",
                 the_tuners_crc_and_unlock_proof_match_the_firmware);
}
