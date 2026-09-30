/* RACCOON V2 - tuner link protocol.
 *
 * The same frames travel over three transports: USB CDC on the bench, CAN between the
 * STM32 and the ESP32-S3, and TCP/IP over WiFi to the tuner. One framing, one CRC, one
 * command set, so a bug cannot hide in one path and not the others.
 *
 * DESIGN NOTES THAT ARE NOT OBVIOUS
 *
 * The frame is length-prefixed and CRC-checked rather than relying on the transport. CAN
 * has no length or checksum beyond its own frame counter, WiFi has a TCP checksum that
 * says nothing about whether the ECU understood anything, and USB bulk transfers can be
 * short. Every transport gets the same guarantee because the guarantee is in the frame.
 *
 * A command is only ever answered once. A retransmitted request therefore produces a
 * duplicate RESPONSE rather than a repeated action - and the one action that must not be
 * repeatable, activating a tune, is additionally non-idempotent in a way the ECU checks
 * (see rc_flash_activate). Retrying is the transport's job, not the application's.
 *
 * The sequence number exists so the ECU can drop stale replies. A tuner on a slow link
 * will have several requests in flight, and a live-data frame that arrives after the
 * operator has moved on is worse than one that never arrives.
 */
#ifndef RC_LINK_H
#define RC_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "cal_types.h"
#include "rc/rc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SOF chosen so it cannot appear in a plausible length or CRC field. */
#define RC_LINK_SOF 0xA5u
#define RC_LINK_VERSION 1u

/** Header, then payload, then CRC-32 over everything before it. Fixed width and
 *  explicitly little-endian on the wire: three implementations have to agree, and
 *  "whatever the compiler did" is not an agreement. */
typedef struct {
    uint8_t sof;        /**< RC_LINK_SOF */
    uint8_t version;    /**< RC_LINK_VERSION */
    uint8_t command;    /**< rc_link_command_t */
    uint8_t flags;
    uint16_t sequence;  /**< request id; replies echo it */
    uint16_t length;    /**< payload bytes */
    /* CRC-32 over the six header bytes above plus the payload. */
} rc_link_header_t;

#define RC_LINK_HEADER_SIZE 8u
#define RC_LINK_MAX_PAYLOAD 512u
/** Header + worst-case payload + CRC. */
#define RC_LINK_MAX_FRAME (RC_LINK_HEADER_SIZE + RC_LINK_MAX_PAYLOAD + 4u)

typedef enum {
    RC_CMD_PING = 0x01,
    RC_CMD_PONG = 0x02,
    RC_CMD_GET_INFO = 0x10,
    RC_CMD_READ_ITEM = 0x11,      /**< one calibration item by id */
    RC_CMD_WRITE_ITEM = 0x12,     /**< one calibration item; validated, never trusted */
    RC_CMD_READ_BULK = 0x13,      /**< the whole image */
    RC_CMD_STAGE = 0x14,
    RC_CMD_VERIFY = 0x15,
    RC_CMD_ACTIVATE = 0x16,
    RC_CMD_ROLLBACK = 0x17,
    RC_CMD_GET_STATUS = 0x20,
    RC_CMD_GET_LIVE = 0x21,       /**< live data, unsolicited at a fixed rate */
    RC_CMD_GET_FAULTS = 0x22,
    RC_CMD_SET_LOGGING = 0x23,
    RC_CMD_AUTHENTICATE = 0x30,   /**< proof of possession before any write is allowed */
    RC_CMD_RESPONSE = 0x7F
} rc_link_command_t;

typedef enum {
    RC_RESP_OK = 0,
    RC_RESP_BAD_CHECKSUM = 1,
    RC_RESP_BAD_COMMAND = 2,
    RC_RESP_BAD_LENGTH = 3,
    RC_RESP_BAD_ID = 4,
    RC_RESP_OUT_OF_RANGE = 5,   /**< the value is outside the item's own range */
    RC_RESP_RULE_VIOLATION = 6, /**< the value breaks a cross-item rule */
    RC_RESP_NOT_AUTHENTICATED = 7,
    RC_RESP_NOT_ARMED = 8,      /**< refused: the engine is running */
    RC_RESP_BUSY = 9,
    RC_RESP_STORAGE = 10,
    RC_RESP_INTERNAL = 11
} rc_link_result_t;

/** Identity, sent once on connect. The tuner shows it, and it is what a log is stamped
 *  with - a datalog whose ECU version is unknown is not evidence of anything. */
typedef struct {
    uint32_t protocol_version;
    uint32_t firmware_version;
    uint32_t image_size;
    uint16_t item_count;
    uint16_t cylinder_count;
    uint32_t flash_active_slot;
    uint32_t flash_active_version;
    uint8_t armed;
    uint8_t flash_rolled_back;
} rc_link_info_t;

/** Live data frame. Deliberately a fixed struct with no padding surprises: this crosses
 *  three implementations, so its layout is asserted at compile time rather than hoped for. */
typedef struct {
    uint32_t sequence;
    uint32_t engine_time_us;
    float rpm;
    float map_kpa;
    float tps_pct;
    float iat_c;
    float ect_c;
    float lambda[RC_MAX_LAMBDA];
    float battery_v;
    float fuel_pressure_kpa;
    float oil_pressure_bar;
    float oil_temp_c;
    float ignition_deg[RC_MAX_IGNITION];
    float injector_ms[RC_MAX_INJECTORS];
    float dwell_ms[RC_MAX_IGNITION];
    uint8_t ignition_cut[RC_MAX_IGNITION];
    uint8_t load_source;
    uint8_t sync_state;
    uint8_t sync_confidence;
    uint8_t lambda_valid[RC_MAX_LAMBDA];
    uint32_t safety_active;
    uint32_t torque_arb_winner;
    float torque_allowed;
    float safety_factor;
    uint8_t armed;
} rc_link_live_t;

/** Where a frame is in the parser. Public so the tests can drive it byte by byte. */
typedef enum {
    RC_PARSE_SOF = 0,
    RC_PARSE_HEADER,
    RC_PARSE_PAYLOAD,
    RC_PARSE_CRC
} rc_parse_state_t;

typedef struct {
    rc_parse_state_t state;
    uint8_t buffer[RC_LINK_MAX_FRAME];
    uint16_t used;
    uint16_t payload_end;   /**< where the CRC begins */
    uint16_t expected;      /**< total frame length including the CRC */
    uint32_t running_crc;
    uint32_t frames_ok;
    uint32_t frames_bad_crc;
    uint32_t frames_dropped;    /**< over the link's own size limit, not merely malformed */
    bool overflowed;
    /** Monotonic microsecond time, set by the caller before parsing. The parser uses it
     *  only for the frame timeout, so it never needs a clock of its own. */
    uint32_t now_us;
    uint32_t frame_start_us;
} rc_link_parser_t;

void rc_link_parser_init(rc_link_parser_t *parser);

/**
 * Feed bytes in. Returns true when a complete, CRC-valid frame is available in
 * `out`/`out_len`, and writes the number of bytes taken from `data` to `consumed`.
 *
 * `consumed` is not optional bookkeeping. A TCP or USB read hands over whatever happened to
 * arrive, which on a busy link is routinely two frames in one buffer, and a parser that
 * returns after the first without saying how much it took silently drops the second - the
 * client then waits forever for a reply the ECU already sent. The caller must resume at
 * `data + *consumed`.
 *
 * The parser is byte-oriented and holds no state beyond the frame in progress, so a reset
 * mid-frame resynchronises on the next SOF.
 */
bool rc_link_parse(rc_link_parser_t *parser, const uint8_t *data, size_t len,
                   rc_link_header_t *out, uint8_t *payload, uint16_t *out_len,
                   size_t *consumed);

/** Give up on a frame that has been arriving for too long, and start hunting for the next
 *  SOF.  Call with a monotonic microsecond time whenever bytes arrive.
 *
 *  This exists because a length field is attacker- and corruption-controlled.  A frame
 *  that declares 500 bytes and then delivers four will otherwise sit in the parser forever,
 *  swallowing every real frame that follows - including, on a track, the operator's next
 *  ACTIVATE.  Resynchronising on every SOF is not an option either, because 0xA5 occurs
 *  inside payloads all the time.  A timeout is the only honest signal that the rest of a
 *  frame is never going to arrive. */
void rc_link_parser_tick(rc_link_parser_t *parser, uint32_t now_us);

/** How long a frame may be in progress before the parser gives up. At the slowest link
 *  that can carry the longest frame, plus margin. */
#define RC_LINK_FRAME_TIMEOUT_US 250000u

/** Serialise a frame. Returns the byte count written, or 0 if the payload is too large. */
uint16_t rc_link_encode(const rc_link_header_t *header, const void *payload, uint16_t len,
                        uint8_t *out, uint16_t out_capacity);

/** Parse a live-data payload.
 *
 *  Returns true only if the payload was complete. On false the output is left UNTOUCHED
 *  rather than zeroed: silently zeroing a live frame is indistinguishable from an engine
 *  that has genuinely reported zeros, and a datalog that cannot tell those apart is not
 *  evidence of anything. */
bool rc_link_decode_live(const uint8_t *payload, uint16_t len, rc_link_live_t *out);

const char *rc_link_command_name(uint8_t command);
const char *rc_link_result_name(int result);

/** Build the live-data payload from the published engine state. Kept here so the
 *  tuner's view of "live" and the datalog's view of "live" cannot drift apart. */
uint16_t rc_link_encode_live(const rc_engine_state_t *state, uint32_t sequence,
                             uint8_t *payload, uint16_t capacity);

#ifdef __cplusplus
}
#endif
#endif /* RC_LINK_H */
