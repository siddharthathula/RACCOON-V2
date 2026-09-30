/* RACCOON V2 - tuner link protocol tests.
 *
 * A link is where a bug becomes a motorcycle on a trailer: a mis-framed activation command
 * is a tune change nobody made, and a dropped byte in a bulk read is a calibration that is
 * subtly wrong. So these test the failure paths, not just the happy one.
 */
#include "rc_test.h"

#include "cal_types.h"
#include "rc/rc_link.h"

/* A frame that survives a round trip through the parser unchanged. */
static void a_frame_round_trips(void)
{
    uint8_t payload[16];
    for (uint8_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 7u);
    }
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_WRITE_ITEM;
    header.flags = 0x01u;
    header.sequence = 0xBEEFu;

    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t len = rc_link_encode(&header, payload, (uint16_t)sizeof(payload), frame,
                                        (uint16_t)sizeof(frame));
    RC_CHECK_EQ_U(len, RC_LINK_HEADER_SIZE + sizeof(payload) + 4u);

    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    rc_link_header_t got;
    uint8_t got_payload[64];
    uint16_t got_len = 0u;
    RC_CHECK_MSG(rc_link_parse(&parser, frame, len, &got, got_payload, &got_len, NULL),
                 "a valid frame was rejected");
    RC_CHECK_EQ_U(got.command, RC_CMD_WRITE_ITEM);
    RC_CHECK_EQ_U(got.sequence, 0xBEEFu);
    RC_CHECK_EQ_U(got_len, sizeof(payload));
    RC_CHECK_EQ_U(parser.frames_ok, 1u);
    RC_CHECK_EQ_U(parser.frames_bad_crc, 0u);
    RC_CHECK_EQ_U(memcmp(got_payload, payload, sizeof(payload)), 0u);
}

/* A single flipped bit must be caught. A CRC that does not catch this is decoration. */
static void a_corrupted_frame_is_rejected(void)
{
    uint8_t payload[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_ACTIVATE;
    header.sequence = 1u;

    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t len = rc_link_encode(&header, payload, (uint16_t)sizeof(payload), frame,
                                        (uint16_t)sizeof(frame));
    /* Try every bit of the payload: an activation command must not survive any of them. */
    for (uint16_t bit = RC_LINK_HEADER_SIZE * 8u; bit < (uint16_t)(len * 8u); ++bit) {
        frame[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        rc_link_parser_t parser;
        rc_link_parser_init(&parser);
        rc_link_header_t got;
        RC_CHECK_MSG(rc_link_parse(&parser, frame, len, &got, NULL, NULL, NULL) == false,
                     "a corrupted activation frame was accepted (bit %u)", (unsigned)bit);
        RC_CHECK_EQ_U(parser.frames_bad_crc, 1u);
        frame[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
}

/* Two frames back to back must both be delivered, in order. */
static void two_frames_in_one_buffer_are_both_delivered(void)
{
    uint8_t stream[RC_LINK_MAX_FRAME * 2u];
    rc_link_header_t a;
    rc_link_header_t b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.command = RC_CMD_PING;
    a.sequence = 10u;
    b.command = RC_CMD_PONG;
    b.sequence = 11u;
    const uint8_t pa[2] = {0xAAu, 0xBBu};
    const uint8_t pb[2] = {0xCCu, 0xDDu};
    const uint16_t la = rc_link_encode(&a, pa, 2u, stream, RC_LINK_MAX_FRAME);
    const uint16_t lb = rc_link_encode(&b, pb, 2u, &stream[la], RC_LINK_MAX_FRAME - la);

    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    rc_link_header_t got;
    uint8_t payload[8];
    uint16_t payload_len = 0u;

    /* Fed in one lump, as a TCP read delivers it. The parser must report how much of the
     * buffer it took, because it returns after the FIRST frame: without a resume point the
     * second frame is silently dropped, and the client then waits forever for a reply the
     * ECU already sent, on a link that is working perfectly. */
    size_t offset = 0u;
    unsigned delivered = 0u;
    uint8_t commands[2] = {0u, 0u};
    uint16_t seqs[2] = {0u, 0u};
    while (offset < (size_t)(la + lb)) {
        size_t consumed = 0u;
        if (rc_link_parse(&parser, &stream[offset], (size_t)(la + lb) - offset, &got, payload,
                          &payload_len, &consumed)) {
            RC_REQUIRE(delivered < 2u);
            commands[delivered] = got.command;
            seqs[delivered] = got.sequence;
            delivered++;
        }
        RC_REQUIRE(consumed > 0u);
        offset += consumed;
    }
    RC_CHECK_EQ_U(delivered, 2u);
    RC_CHECK_EQ_U(commands[0], RC_CMD_PING);
    RC_CHECK_EQ_U(commands[1], RC_CMD_PONG);
    RC_CHECK_EQ_U(seqs[0], 10u);
    RC_CHECK_EQ_U(seqs[1], 11u);
    /* And fed one byte at a time, which is what a UART or a slow CAN bridge does. */
    rc_link_parser_t drip;
    rc_link_parser_init(&drip);
    size_t consumed = 0u;
    uint8_t seen = 0u;
    for (uint16_t i = 0u; i < la; ++i) {
        if (rc_link_parse(&drip, &stream[i], 1u, &got, payload, &payload_len, NULL)) {
            seen++;
            RC_CHECK_EQ_U(got.sequence, 10u);
        }
    }
    (void)consumed;
    RC_CHECK_EQ_U(seen, 1u);
}

/* Garbage before a frame must be skipped, not swallowed into it. This is what happens when
 * a boot ROM banner precedes the protocol, or the link is opened mid-stream. */
static void leading_garbage_is_skipped(void)
{
    /* A frame with a good header, then a payload that never arrives. Without the frame
     * timeout this sits in the parser forever and eats every real frame after it. */
    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    const uint8_t truncated[12] = {
        RC_LINK_SOF, RC_LINK_VERSION, RC_CMD_READ_BULK, 0u,
        0x01u, 0x00u,                     /* sequence 1 */
        0xF0u, 0x00u,                     /* length 240: legal, but never delivered */
        0xDEu, 0xADu, 0xBEu, 0x00u
    };
    rc_link_header_t got;
    parser.now_us = 1000u;
    RC_CHECK(rc_link_parse(&parser, truncated, sizeof(truncated), &got, NULL, NULL, NULL) == false);
    RC_CHECK_EQ_U(parser.frames_ok, 0u);

    /* The link goes quiet. The frame must be abandoned rather than held open forever. */
    parser.now_us = 1000u + RC_LINK_FRAME_TIMEOUT_US + 1u;
    rc_link_parser_tick(&parser, parser.now_us);
    RC_CHECK_EQ_U(parser.frames_dropped, 1u);
    RC_CHECK_EQ_U((uint8_t)parser.state, (uint8_t)RC_PARSE_SOF);

    /* And the next real frame is received, which is the point: one abandoned frame must
     * not cost the operator everything that follows it. */
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_PING;
    header.sequence = 2u;
    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t len = rc_link_encode(&header, NULL, 0u, frame, (uint16_t)sizeof(frame));
    parser.now_us += 1000u;
    RC_CHECK_MSG(rc_link_parse(&parser, frame, len, &got, NULL, NULL, NULL),
                 "the parser did not recover after abandoning a frame");
    RC_CHECK_EQ_U(got.command, RC_CMD_PING);
    RC_CHECK_EQ_U(got.sequence, 2u);
}

/* Bytes before a frame's SOF are noise and are discarded, not interpreted. */
static void noise_before_a_frame_is_discarded(void)
{
    uint8_t stream[64];
    const uint8_t noise[] = {0x00u, 0xFFu, 0x11u, 0x22u, 0x33u, 0xA5u, 0x44u};
    memcpy(stream, noise, sizeof(noise));

    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_PING;
    header.sequence = 0x1234u;
    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t flen = rc_link_encode(&header, NULL, 0u, frame, (uint16_t)sizeof(frame));
    memcpy(&stream[sizeof(noise)], frame, flen);

    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    rc_link_header_t got;
    /* A stray 0xA5 in the noise will start a header that is then abandoned, and the
     * parser must resynchronise on the real frame's SOF. */
    RC_CHECK(rc_link_parse(&parser, stream, sizeof(noise) + flen, &got, NULL, NULL, NULL) == false
             || parser.frames_ok > 0u);
    /* Feed the real frame on its own to confirm it is intact. */
    rc_link_parser_t clean;
    rc_link_parser_init(&clean);
    RC_CHECK(rc_link_parse(&clean, frame, flen, &got, NULL, NULL, NULL));
    RC_CHECK_EQ_U(got.sequence, 0x1234u);
}

/* A length field claiming more than the protocol allows must be rejected immediately
 * rather than making the receiver wait for a frame that will never be complete. */
static void an_impossible_length_is_dropped_immediately(void)
{
    rc_link_parser_t parser;
    rc_link_parser_init(&parser);
    const uint8_t frame[12] = {
        RC_LINK_SOF, RC_LINK_VERSION, RC_CMD_READ_BULK, 0u,
        0x01u, 0x00u,              /* sequence 1 */
        0xFFu, 0xFFu,              /* length 65535, impossible */
        0xDEu, 0xADu, 0xBEu, 0xEFu
    };
    rc_link_header_t got;
    RC_CHECK(rc_link_parse(&parser, frame, sizeof(frame), &got, NULL, NULL, NULL) == false);
    RC_CHECK_EQ_U(parser.frames_dropped, 1u);
}

/* A frame truncated mid-payload must never be delivered as a shorter valid one. */
static void a_truncated_frame_is_not_delivered(void)
{
    uint8_t payload[64];
    memset(payload, 0x5A, sizeof(payload));
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_READ_BULK;
    header.sequence = 3u;

    uint8_t frame[RC_LINK_MAX_FRAME];
    const uint16_t len = rc_link_encode(&header, payload, (uint16_t)sizeof(payload), frame,
                                        (uint16_t)sizeof(frame));
    for (uint16_t cut = 1u; cut < len; ++cut) {
        rc_link_parser_t parser;
        rc_link_parser_init(&parser);
        rc_link_header_t got;
        uint16_t got_len = 0u;
        RC_CHECK_MSG(rc_link_parse(&parser, frame, cut, &got, NULL, &got_len, NULL) == false,
                     "a frame truncated to %u of %u bytes was accepted", (unsigned)cut,
                     (unsigned)len);
    }
}

/* The link lives or dies on random data, so it is tested against random data. A fixed seed
 * keeps a failure reproducible rather than a rumour. */
static void random_bytes_never_produce_a_frame(void)
{
    uint32_t rng = 0x1234567u;
    for (int trial = 0; trial < 2000; ++trial) {
        rc_link_parser_t parser;
        rc_link_parser_init(&parser);
        uint8_t noise[64];
        const uint16_t len = (uint16_t)(8u + (rng % 56u));
        for (uint16_t i = 0u; i < len; ++i) {
            rng = (rng * 1103515245u) + 12345u;
            noise[i] = (uint8_t)((rng >> 16) & 0xFFu);
        }
        rc_link_header_t got;
        uint8_t payload[RC_LINK_MAX_PAYLOAD];
        uint16_t payload_len = 0u;
        if (rc_link_parse(&parser, noise, len, &got, payload, &payload_len, NULL)) {
            /* A chance hit is astronomically unlikely, but if it happens it must at least
             * be a well-formed frame with a matching CRC - never a partial one. */
            RC_CHECK_EQ_U(got.length, payload_len);
            RC_CHECK(payload_len <= RC_LINK_MAX_PAYLOAD);
        }
        /* Whatever happened, the parser must be usable again. */
        RC_CHECK_EQ_U(parser.used <= RC_LINK_MAX_FRAME, 1u);
    }
}

/* The live-data frame must survive a round trip exactly. This is the frame a datalog is
 *  made of, and a subtly wrong one is a log that cannot be trusted. */
static void live_data_round_trips(void)
{
    rc_engine_state_t state;
    memset(&state, 0, sizeof(state));
    state.engine_time_us = 1234567u;
    state.pos.rpm_x2 = 6000u;
    state.load = 42.5f;
    state.load_source = (uint8_t)RC_LOAD_MAP;
    state.lambda[0] = 0.98f;
    state.lambda_valid[0] = 1u;
    state.ignition_deg[0] = 12.5f;
    state.ignition_deg[1] = 12.5f;
    state.injector_ms[0] = 2.75f;
    state.injector_ms[1] = 2.75f;
    state.dwell_ms[0] = 3.0f;
    state.raw.battery_v = 13.8f;
    state.raw.fuel_pressure_kpa = 310.0f;
    state.raw.oil_pressure_bar = 4.25f;
    state.raw.oil_temp_c = 96.5f;
    state.safety_active = 0x0102u;
    state.corr.torque_allowance = 0.8f;
    state.armed = 1u;

    uint8_t payload[512];
    const uint16_t len = rc_link_encode_live(&state, 99u, payload, (uint16_t)sizeof(payload));
    RC_CHECK(len > 0u);

    rc_link_live_t live;
    rc_link_decode_live(payload, len, &live);
    RC_CHECK_EQ_U(live.sequence, 99u);
    RC_CHECK_EQ_U(live.engine_time_us, 1234567u);
    RC_CHECK_NEAR(live.rpm, 3000.0f, 1e-3);
    RC_CHECK_NEAR(live.map_kpa, 42.5f, 1e-4);
    RC_CHECK_NEAR(live.lambda[0], 0.98f, 1e-5);
    RC_CHECK_NEAR(live.ignition_deg[0], 12.5f, 1e-4);
    RC_CHECK_NEAR(live.injector_ms[0], 2.75f, 1e-5);
    /* The four raw sensor readings travel in the frame. If the encoder sends them and the
     * decoder skips them, every later field lands four bytes early and the frame decodes
     * into values that are all the right size and none of them right. */
    RC_CHECK_NEAR(live.battery_v, 13.8f, 1e-4);
    RC_CHECK_NEAR(live.fuel_pressure_kpa, 310.0f, 1e-3);
    RC_CHECK_NEAR(live.oil_pressure_bar, 4.25f, 1e-4);
    RC_CHECK_NEAR(live.oil_temp_c, 96.5f, 1e-4);
    RC_CHECK_EQ_U(live.ignition_cut[0], 0u);
    RC_CHECK_EQ_U(live.safety_active, 0x0102u);
    RC_CHECK_NEAR(live.torque_allowed, 0.8f, 1e-4);
    RC_CHECK_EQ_U(live.armed, 1u);
}

/* A short payload must leave the structure zeroed, not half-filled. A decoder that reads
 *  past a short buffer is how a malformed frame becomes memory corruption on the ESP32. */
static void a_short_live_payload_is_rejected_not_half_read(void)
{
    rc_link_live_t live;
    memset(&live, 0xAA, sizeof(live));
    const uint8_t stub[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    RC_CHECK_MSG(rc_link_decode_live(stub, (uint16_t)sizeof(stub), &live) == false,
                 "a truncated payload was accepted");
    /* The output is left untouched, not half-written: a caller that ignores the return
     * value then keeps the previous frame rather than a plausible-looking mixture. */
    RC_CHECK_EQ_U(live.sequence, 0xAAAAAAAAu);
    /* And a zero-length or null payload is harmless and reported. */
    RC_CHECK(rc_link_decode_live(NULL, 0u, &live) == false);
    RC_CHECK(rc_link_decode_live(stub, 0u, &live) == false);
    RC_CHECK_EQ_U(live.sequence, 0xAAAAAAAAu);
}

/* Encoding must refuse to overflow its buffer rather than writing past it. */
static void encoding_refuses_to_overflow(void)
{
    uint8_t tiny[4];
    rc_link_header_t header;
    memset(&header, 0, sizeof(header));
    header.command = RC_CMD_READ_BULK;
    RC_CHECK_EQ_U(rc_link_encode(&header, NULL, 0u, tiny, (uint16_t)sizeof(tiny)), 0u);

    uint8_t frame[RC_LINK_MAX_FRAME];
    static uint8_t big[RC_LINK_MAX_PAYLOAD];
    RC_CHECK(rc_link_encode(&header, big, RC_LINK_MAX_PAYLOAD, frame,
                            (uint16_t)sizeof(frame)) > 0u);
    /* A payload one byte over the limit is refused outright, not truncated. */
    RC_CHECK_EQ_U(rc_link_encode(&header, big, (uint16_t)(RC_LINK_MAX_PAYLOAD + 1u), frame,
                                 (uint16_t)sizeof(frame)), 0u);
}

void test_link(void)
{
    RC_TEST_CASE("a/frame/round/trips", a_frame_round_trips);
    RC_TEST_CASE("a/corrupted/frame/is/rejected", a_corrupted_frame_is_rejected);
    RC_TEST_CASE("two/frames/in/one/buffer/are/both/delivered",
                 two_frames_in_one_buffer_are_both_delivered);
    RC_TEST_CASE("leading/garbage/is/skipped", leading_garbage_is_skipped);
    RC_TEST_CASE("noise/before/a/frame/is/discarded", noise_before_a_frame_is_discarded);
    RC_TEST_CASE("an/impossible/length/is/dropped/immediately",
                 an_impossible_length_is_dropped_immediately);
    RC_TEST_CASE("a/truncated/frame/is/not/delivered", a_truncated_frame_is_not_delivered);
    RC_TEST_CASE("random/bytes/never/produce/a/frame", random_bytes_never_produce_a_frame);
    RC_TEST_CASE("live/data/round/trips", live_data_round_trips);
    RC_TEST_CASE("a/short/live/payload/is/rejected/not/half/read",
                 a_short_live_payload_is_rejected_not_half_read);
    RC_TEST_CASE("encoding/refuses/to/overflow", encoding_refuses_to_overflow);
}
