/* RACCOON V2 - tuner link protocol.  See rc_link.h. */
#include "rc/rc_link.h"

#include <string.h>

#include "rc/rc_crc32.h"

/* The running frame checksum.
 *
 *  The library's rc_crc32() is the post-conditioned CRC-32 the host tooling expects, so it
 *  applies its final XOR on every call. Chaining it byte by byte therefore does NOT give
 *  the same answer as computing it over the whole buffer in one go - the XOR is applied
 *  repeatedly. Both the encoder and the decoder here would then be self-consistent and
 *  every frame would still be rejected by a host that computes the standard checksum, and
 *  the fault would look like a driver problem rather than an arithmetic one.
 *
 *  So the frame checksum is a raw running value, with the conditioning applied once, at
 *  the end, on both sides. */
#define RC_LINK_CRC_POLY 0xEDB88320u
/* Both conditioning values. CRC-32 as everyone computes it - zlib, the FIPS appendix, the
 * tuner's own library - pre-conditions the register with all ones and post-conditions it
 * with all ones. A register that starts at zero instead computes JAMCRC, which is a
 * perfectly good checksum and disagrees with every other implementation. The symptom is
 * that the two ends of the link each validate their own frames and neither validates the
 * other's, and nothing at all looks broken until it is plugged into a real ECU. */
#define RC_LINK_CRC_INIT 0xFFFFFFFFu
#define RC_LINK_CRC_XOROUT 0xFFFFFFFFu

/** CRC-32 over a running, already-pre-conditioned value. */
static uint32_t rc_crc32_raw(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ RC_LINK_CRC_POLY) : (crc >> 1);
        }
    }
    return crc;
}

/* The wire format is byte-order-explicit. Three implementations have to agree without
 * being compiled by the same compiler, so nothing is memcpy'd onto the wire. */
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

static void put_f32(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    put_u32(p, bits);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static float get_f32(const uint8_t *p)
{
    const uint32_t bits = get_u32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void rc_link_parser_init(rc_link_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->state = RC_PARSE_SOF;
}

uint16_t rc_link_encode(const rc_link_header_t *header, const void *payload, uint16_t len,
                        uint8_t *out, uint16_t out_capacity)
{
    if (header == NULL || out == NULL || len > RC_LINK_MAX_PAYLOAD) {
        return 0u;
    }
    const uint16_t total = (uint16_t)(RC_LINK_HEADER_SIZE + len + 4u);
    if (total > out_capacity || total > RC_LINK_MAX_FRAME) {
        return 0u;
    }
    out[0] = RC_LINK_SOF;
    out[1] = RC_LINK_VERSION;
    out[2] = header->command;
    out[3] = header->flags;
    put_u16(&out[4], header->sequence);
    put_u16(&out[6], len);
    if (len > 0u && payload != NULL) {
        memcpy(&out[RC_LINK_HEADER_SIZE], payload, len);
    }
    /* CRC over everything that came before it, so a truncated or corrupted frame is
     * detected by the receiver rather than being interpreted as a short valid frame. */
    /* The transmitted value carries the same final conditioning the receiver applies when
     * it checks. Getting this wrong on one side only produces a link where every frame is
     * rejected and the fault appears to be in the checksum rather than in the arithmetic. */
    const uint32_t crc = rc_crc32_raw(RC_LINK_CRC_INIT, out,
                                      (size_t)(RC_LINK_HEADER_SIZE + len)) ^ RC_LINK_CRC_XOROUT;
    put_u32(&out[RC_LINK_HEADER_SIZE + len], crc);
    return total;
}

void rc_link_parser_tick(rc_link_parser_t *parser, uint32_t now_us)
{
    if (parser == NULL || parser->state == RC_PARSE_SOF) {
        return;
    }
    if (parser->frame_start_us == 0u) {
        parser->frame_start_us = now_us;
        return;
    }
    if ((uint32_t)(now_us - parser->frame_start_us) > RC_LINK_FRAME_TIMEOUT_US) {
        parser->frames_dropped++;
        parser->state = RC_PARSE_SOF;
        parser->used = 0u;
        parser->frame_start_us = 0u;
    }
}

bool rc_link_parse(rc_link_parser_t *parser, const uint8_t *data, size_t len,
                   rc_link_header_t *out, uint8_t *payload, uint16_t *out_len,
                   size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0u;
    }
    if (parser == NULL || data == NULL) {
        return false;
    }
    if (parser->now_us != 0u) {
        rc_link_parser_tick(parser, parser->now_us);
    }
    for (size_t i = 0u; i < len; ++i) {
        /* Recorded as the loop goes, and published on every exit path, so the caller's
         * resume point is always exactly what was looked at. */
        if (consumed != NULL) {
            *consumed = i + 1u;
        }
        const uint8_t byte = data[i];
        switch (parser->state) {
            case RC_PARSE_SOF:
                if (byte == RC_LINK_SOF) {
                    parser->buffer[0] = byte;
                    parser->used = 1u;
                    parser->frame_start_us = parser->now_us;
                    /* Each frame's checksum starts from zero. Carrying the previous
                     * frame's running value in would make every frame after an abandoned
                     * one fail, and the symptom - "the link works until something goes
                     * wrong, then it never works again" - points nowhere near the cause. */
                    parser->running_crc = rc_crc32_raw(RC_LINK_CRC_INIT, &byte, 1u);
                    parser->state = RC_PARSE_HEADER;
                }
                /* Any other byte is noise on the line and is discarded. Resynchronising on
                 * SOF rather than on a timeout is what lets this survive a link that
                 * drops a byte in the middle of a frame. */
                break;

            case RC_PARSE_HEADER:
                parser->buffer[parser->used++] = byte;
                parser->running_crc = rc_crc32_raw(parser->running_crc, &byte, 1u);
                if (parser->used >= RC_LINK_HEADER_SIZE) {
                    const uint16_t payload_len = get_u16(&parser->buffer[6]);
                    if (payload_len > RC_LINK_MAX_PAYLOAD) {
                        /* Not a length this protocol can produce. The frame is discarded
                         * and the parser goes back to hunting for the next SOF, rather than
                         * allocating for or waiting on a length that is already wrong. */
                        parser->frames_dropped++;
                        parser->state = RC_PARSE_SOF;
                        parser->used = 0u;
                        parser->frame_start_us = 0u;
                        break;
                    }
                    parser->payload_end = (uint16_t)(RC_LINK_HEADER_SIZE + payload_len);
                    parser->expected = (uint16_t)(parser->payload_end + 4u);
                    parser->state = RC_PARSE_PAYLOAD;
                }
                break;

            case RC_PARSE_PAYLOAD:
            case RC_PARSE_CRC:
                parser->buffer[parser->used++] = byte;
                /* The running CRC covers the header and payload only. The four trailing
                 * bytes ARE the CRC and must never be folded into it. The test is made
                 * BEFORE the byte is folded, not after: doing it the other way round
                 * includes the first CRC byte, which disagrees with the sender by exactly
                 * that byte and rejects every single frame. */
                /* `used` now counts this byte, so this byte belongs to the payload exactly
                 * when used <= payload_end. Getting that comparison wrong by one either
                 * swallows the first CRC byte or drops the last payload byte, and both
                 * present as "every frame is rejected" with nothing wrong in sight. */
                if (parser->state == RC_PARSE_PAYLOAD
                    && parser->used > parser->payload_end) {
                    parser->state = RC_PARSE_CRC;
                } else if (parser->state == RC_PARSE_PAYLOAD) {
                    parser->running_crc = rc_crc32_raw(parser->running_crc, &byte, 1u);
                }
                if (parser->used >= parser->expected) {
                    const uint32_t received = get_u32(&parser->buffer[parser->expected - 4u]);
                    parser->used = 0u;
                    parser->state = RC_PARSE_SOF;
                    if (received != (parser->running_crc ^ RC_LINK_CRC_XOROUT)) {
                        parser->frames_bad_crc++;
                        break; /* silently drop: the caller is told by the absence of a frame */
                    }
                    if (out != NULL) {
                        out->sof = parser->buffer[0];
                        out->version = parser->buffer[1];
                        out->command = parser->buffer[2];
                        out->flags = parser->buffer[3];
                        out->sequence = get_u16(&parser->buffer[4]);
                        out->length = get_u16(&parser->buffer[6]);
                    }
                    const uint16_t payload_len = get_u16(&parser->buffer[6]);
                    if (payload != NULL && payload_len > 0u) {
                        memcpy(payload, &parser->buffer[RC_LINK_HEADER_SIZE], payload_len);
                    }
                    if (out_len != NULL) {
                        *out_len = payload_len;
                    }
                    parser->frames_ok++;
                    parser->frame_start_us = 0u;
                    return true;
                }
                break;

            default:
                rc_link_parser_init(parser);
                break;
        }
    }
    return false;
}

uint16_t rc_link_encode_live(const rc_engine_state_t *state, uint32_t sequence,
                             uint8_t *payload, uint16_t capacity)
{
    if (state == NULL || payload == NULL) {
        return 0u;
    }
    uint16_t n = 0u;
#define PUT_U32(v) do { if ((uint16_t)(n + 4u) > capacity) { return 0u; } put_u32(&payload[n], (v)); n = (uint16_t)(n + 4u); } while (0)
#define PUT_U16(v) do { if ((uint16_t)(n + 2u) > capacity) { return 0u; } put_u16(&payload[n], (v)); n = (uint16_t)(n + 2u); } while (0)
#define PUT_U8(v)  do { if ((uint16_t)(n + 1u) > capacity) { return 0u; } payload[n] = (uint8_t)(v); n = (uint16_t)(n + 1u); } while (0)
#define PUT_F32(v) do { if ((uint16_t)(n + 4u) > capacity) { return 0u; } put_f32(&payload[n], (v)); n = (uint16_t)(n + 4u); } while (0)

    PUT_U32(sequence);
    PUT_U32(state->engine_time_us);
    PUT_F32((float)state->pos.rpm_x2 * 0.5f);
    PUT_F32(state->load);
    PUT_F32(state->raw.tps_pct);
    PUT_F32(state->raw.iat_c);
    PUT_F32(state->raw.ect_c);
    for (uint8_t i = 0u; i < RC_MAX_LAMBDA; ++i) {
        PUT_F32(state->lambda[i]);
    }
    PUT_U8(state->load_source);
    PUT_U8((uint8_t)state->pos.sync_state);
    PUT_U8(state->pos.sync_confidence);
    for (uint8_t i = 0u; i < RC_MAX_LAMBDA; ++i) {
        PUT_U8(state->lambda_valid[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        PUT_F32(state->ignition_deg[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_INJECTORS; ++i) {
        PUT_F32(state->injector_ms[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        PUT_F32(state->dwell_ms[i]);
    }
    PUT_F32(state->raw.battery_v);
    PUT_F32(state->raw.fuel_pressure_kpa);
    PUT_F32(state->raw.oil_pressure_bar);
    PUT_F32(state->raw.oil_temp_c);
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        PUT_U8(state->ignition_cut[i]);
    }
    PUT_U32(state->safety_active);
    PUT_U32(state->arb_trace);
    PUT_F32(state->corr.torque_allowance);
    PUT_F32(state->corr.safety_factor);
    PUT_U8(state->armed);

#undef PUT_U32
#undef PUT_U16
#undef PUT_U8
#undef PUT_F32
    return n;
}

bool rc_link_decode_live(const uint8_t *payload, uint16_t len, rc_link_live_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    /* Decode into a local and only publish on completion. Filling the caller's structure
     * field by field and bailing out half way leaves a structure with a valid sequence
     * number and garbage everything else - which the caller cannot distinguish from real
     * data, and which looks like a working log of an event that never happened. A wire
     * decode is all or nothing. */
    rc_link_live_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    uint16_t n = 0u;
    /* Bounded on every read: a short payload from a truncated or hostile frame must leave
     * the structure zeroed rather than half-populated, and must never read past the end. */
#define NEED(k) do { if ((uint16_t)(n + (k)) > len) { return false; } } while (0)
#define GET_U32(dst) do { NEED(4u); (dst) = get_u32(&payload[n]); n = (uint16_t)(n + 4u); } while (0)
#define GET_U8(dst)  do { NEED(1u); (dst) = payload[n]; n = (uint16_t)(n + 1u); } while (0)
#define GET_F32(dst) do { NEED(4u); (dst) = get_f32(&payload[n]); n = (uint16_t)(n + 4u); } while (0)

    GET_U32(parsed.sequence);
    GET_U32(parsed.engine_time_us);
    GET_F32(parsed.rpm);
    GET_F32(parsed.map_kpa);
    GET_F32(parsed.tps_pct);
    GET_F32(parsed.iat_c);
    GET_F32(parsed.ect_c);
    for (uint8_t i = 0u; i < RC_MAX_LAMBDA; ++i) {
        GET_F32(parsed.lambda[i]);
    }
    GET_U8(parsed.load_source);
    GET_U8(parsed.sync_state);
    GET_U8(parsed.sync_confidence);
    for (uint8_t i = 0u; i < RC_MAX_LAMBDA; ++i) {
        GET_U8(parsed.lambda_valid[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        GET_F32(parsed.ignition_deg[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_INJECTORS; ++i) {
        GET_F32(parsed.injector_ms[i]);
    }
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        GET_F32(parsed.dwell_ms[i]);
    }
    GET_F32(parsed.battery_v);
    GET_F32(parsed.fuel_pressure_kpa);
    GET_F32(parsed.oil_pressure_bar);
    GET_F32(parsed.oil_temp_c);
    for (uint8_t i = 0u; i < RC_MAX_IGNITION; ++i) {
        GET_U8(parsed.ignition_cut[i]);
    }
    GET_U32(parsed.safety_active);
    GET_U32(parsed.torque_arb_winner);
    GET_F32(parsed.torque_allowed);
    GET_F32(parsed.safety_factor);
    GET_U8(parsed.armed);
    /* Complete: publish. */
    *out = parsed;
    return true;
#undef NEED
#undef GET_U32
#undef GET_U8
#undef GET_F32
}

const char *rc_link_command_name(uint8_t command)
{
    switch (command) {
        case RC_CMD_PING: return "ping";
        case RC_CMD_PONG: return "pong";
        case RC_CMD_GET_INFO: return "get_info";
        case RC_CMD_READ_ITEM: return "read_item";
        case RC_CMD_WRITE_ITEM: return "write_item";
        case RC_CMD_READ_BULK: return "read_bulk";
        case RC_CMD_STAGE: return "stage";
        case RC_CMD_VERIFY: return "verify";
        case RC_CMD_ACTIVATE: return "activate";
        case RC_CMD_ROLLBACK: return "rollback";
        case RC_CMD_GET_STATUS: return "get_status";
        case RC_CMD_GET_LIVE: return "get_live";
        case RC_CMD_GET_FAULTS: return "get_faults";
        case RC_CMD_SET_LOGGING: return "set_logging";
        case RC_CMD_AUTHENTICATE: return "authenticate";
        case RC_CMD_RESPONSE: return "response";
        default: return "an unrecognised command";
    }
}

const char *rc_link_result_name(int result)
{
    switch (result) {
        case RC_RESP_OK: return "ok";
        case RC_RESP_BAD_CHECKSUM: return "the frame failed its checksum";
        case RC_RESP_BAD_COMMAND: return "unrecognised command";
        case RC_RESP_BAD_LENGTH: return "wrong payload length";
        case RC_RESP_BAD_ID: return "no such calibration item";
        case RC_RESP_OUT_OF_RANGE: return "the value is outside that item's range";
        case RC_RESP_RULE_VIOLATION: return "the value breaks a calibration rule";
        case RC_RESP_NOT_AUTHENTICATED: return "not authenticated";
        case RC_RESP_NOT_ARMED: return "refused: the engine is running";
        case RC_RESP_BUSY: return "busy";
        case RC_RESP_STORAGE: return "the storage operation failed";
        case RC_RESP_INTERNAL: return "internal error";
        default: return "an unrecognised result";
    }
}
