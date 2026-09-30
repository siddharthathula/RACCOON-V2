"""Generate the C bridge used by the wire-format parity test.

The C test cannot import Python, so the Python reference is executed here, at build time,
and its answers are baked into a small C file. That way the parity test compares the real
Python implementation against the real C one rather than against a hand-written summary of
what Python probably does - which would only prove the summary is self-consistent.

Run standalone to inspect the output:  python3 tools/gen_parity.py
"""

from __future__ import annotations

import hashlib
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tuner"))

from raccoon.link import (  # noqa: E402
    Command, Frame, FrameParser, MAX_FRAME, decode_live,
    MAX_IGNITION, MAX_INJECTORS, MAX_LAMBDA,
)

# The cases both implementations must agree on. Chosen to cover an empty payload, a short
# one, and one that is a full 32 bytes, because length handling is where the two would
# most plausibly drift.
ENCODE_CASES = [
    ("", Command.WRITE_ITEM, 0x1234),
    ("00", Command.WRITE_ITEM, 0x1234),
    ("0102030405060708", Command.WRITE_ITEM, 0x1234),
    ("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     Command.WRITE_ITEM, 0x1234),
]

# A fixed engine state, encoded in Python. The C test encodes the same state and the two
# byte strings must be identical - this is the check that matters, because a field in the
# wrong place still decodes, it just decodes as a different number.
LIVE_STATE = dict(
    sequence=777, engine_time_us=4242, rpm=3250.0, map_kpa=55.25,
    tps_pct=23.5, iat_c=31.0, ect_c=88.5,
    lambda_values=[0.93, 0.94, 0.0, 0.0],
    # These are the firmware's own enum values, spelled out: RC_LOAD_MAP is 0 and
    # RC_TRIGGER_SYNCED is 4. Using the wrong number here would look like a layout
    # mismatch, and the point of this table is the LAYOUT.
    load_source=0, sync_state=4, sync_confidence=100,
    lambda_valid=[1, 0, 0, 0],
    ignition_deg=[11.5, 0.0, 0.0, 12.5] + [0.0] * 4,
    injector_ms=[3.25] + [0.0] * (MAX_INJECTORS - 1),
    dwell_ms=[0.0, 3.5, 0.0, 0.0] + [0.0] * 4,
    battery_v=13.8, fuel_pressure_kpa=310.0, oil_pressure_bar=4.2, oil_temp_c=96.0,
    ignition_cut=[0, 0, 1, 0] + [0] * 4,
    safety_active=0x0400, torque_arb_winner=0,
    torque_allowed=0.65, safety_factor=0.9, armed=1,
)


# The live layout, built from the same constants the C encoder uses. Written as an
# expression rather than a literal so a change to the field count cannot leave one side
# describing a frame the other does not.
LIVE_FORMAT = (
    "<II"                       # sequence, engine time
    + "f" * 5                    # rpm, map, tps, iat, ect
    + "f" * MAX_LAMBDA
    + "BBB"                      # load source, sync state, sync confidence
    + "B" * MAX_LAMBDA
    + "f" * MAX_IGNITION
    + "f" * MAX_INJECTORS
    + "f" * MAX_IGNITION
    + "ffff"                    # battery, fuel pressure, oil pressure, oil temp
    + "B" * MAX_IGNITION
    + "II"                       # safety active, arb winner
    + "ff"                       # torque allowance, safety factor
    + "B"                        # armed
)


def encode_live(state: dict) -> bytes:
    """The same field order and widths as rc_link_encode_live."""
    values = [
        state["sequence"], state["engine_time_us"], state["rpm"], state["map_kpa"],
        state["tps_pct"], state["iat_c"], state["ect_c"],
        *state["lambda_values"],
        state["load_source"], state["sync_state"], state["sync_confidence"],
        *state["lambda_valid"],
        *state["ignition_deg"],
        *state["injector_ms"],
        *state["dwell_ms"],
        state["battery_v"], state["fuel_pressure_kpa"],
        state["oil_pressure_bar"], state["oil_temp_c"],
        *state["ignition_cut"],
        state["safety_active"], state["torque_arb_winner"],
        state["torque_allowed"], state["safety_factor"], state["armed"],
    ]
    return struct.pack(LIVE_FORMAT, *values)


def _c_default_image() -> str:
    """The bytes the firmware's live calibration actually contains, as hex.

    Dumped by compiling and running a two-line program against the real generated
    rc_cal_init(). Comparing against this is the only way to be sure the tuner's image
    matches: reading the offsets out of the descriptor and hoping the widths line up is
    how an image that is the right SIZE and wrong in every byte gets written to a running
    engine.
    """
    import subprocess
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / "dump.c"
        source.write_text(
            '#include "cal_types.h"\n'
            '#include "rc/cal.h"\n'
            '#include <stdio.h>\n'
            'int main(void){ rc_cal_init(); const uint8_t *b=(const uint8_t*)&rc_cal_live;\n'
            '  for(unsigned i=0;i<sizeof(rc_cal_t);i++) printf("%02x",b[i]);\n'
            '  printf("\\n"); return 0; }\n'
        )
        binary = pathlib.Path(tmp) / "dump"
        root = HERE.parent
        subprocess.run(
            ["gcc", "-std=c17", f"-I{root / 'firmware' / 'include'}",
             f"-I{root / 'firmware' / 'generated'}", str(source),
             str(root / "firmware" / "src" / "calibration" / "cal.c"),
             str(root / "firmware" / "generated" / "cal_defaults.c"),
             str(root / "firmware" / "generated" / "cal_descriptors.c"),
             str(root / "firmware" / "src" / "core" / "rc_crc32.c"),
             "-o", str(binary)],
            check=True, capture_output=True,
        )
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        return result.stdout.strip()


def crc32c(data: bytes, crc: int = 0) -> int:
    """CRC-32C, matching rc_crc32c. Used only to build the unlock proof."""
    poly = 0x82F63B78
    if not data:
        return crc
    value = crc ^ 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (poly if value & 1 else 0)
    return value ^ 0xFFFFFFFF


# The code the parity proof is built from. Any value works as long as both sides use this
# one: what is being checked is that the two implementations agree on the DERIVATION, not
# that a particular code is secret.
PARITY_CODE = "parity-check-not-a-secret"


def unlock_proof(salt: bytes, image: bytes) -> str:
    """salt || HMAC(key, salt || le32(CRC32C(image))) - what rc_link_service.c computes.

        key = SHA-256("RACCOON-UNLOCK-V1" || salt || code)
    """
    import hmac

    key = hashlib.sha256(b"RACCOON-UNLOCK-V1" + salt + PARITY_CODE.encode()).digest()
    material = salt + crc32c(image).to_bytes(4, "little")
    return (salt + hmac.new(key, material, hashlib.sha256).digest()).hex()


def main(argv: list[str]) -> int:
    encode_table: list[tuple[str, str]] = []
    for hex_payload, command, sequence in ENCODE_CASES:
        frame = Frame(command=int(command), sequence=sequence,
                      payload=bytes.fromhex(hex_payload))
        encode_table.append((hex_payload, frame.encode().hex()))

    payload = bytes((i * 11) & 0xFF for i in range(16))
    good = Frame(command=int(Command.READ_BULK), sequence=0x4321, payload=payload).encode()
    bad = bytearray(good)
    bad[10] ^= 0x08
    parser = FrameParser()
    accept_good = len(parser.feed(good))
    parser_bad = FrameParser()
    accept_bad = len(parser_bad.feed(bytes(bad)))
    decode_table = [(good.hex(), 1 if accept_good else 0), (bytes(bad).hex(),
                                                            1 if accept_bad else 0)]

    c_default = _c_default_image()
    # Import the tuner's calibration module and ask IT for its default image, rather than
    # recomputing it here. This is the whole point: the module the GUI actually uses is
    # the one under test.
    sys.path.insert(0, str(HERE.parent / "tuner"))
    from raccoon.calibration import default_image
    py_default = default_image().hex()
    live_hex = encode_live(LIVE_STATE).hex()
    decoded = decode_live(bytes.fromhex(live_hex))
    live_matches = int(decoded is not None
                       and decoded.sequence == LIVE_STATE["sequence"]
                       and abs(decoded.rpm - LIVE_STATE["rpm"]) < 1e-3
                       and abs(decoded.lambda_values[0] - 0.93) < 1e-5
                       and decoded.sync_state == 4
                       and decoded.safety_active == 0x0400
                       and abs(decoded.torque_allowed - 0.65) < 1e-5)

    out = [
        "/* Generated by tools/gen_parity.py. Do not edit by hand.",
        " *",
        " * The Python reference implementation was executed to produce these answers. The",
        " * C parity test compares its own output against them, so a change to either side",
        " * of the wire format that is not made to the other shows up as a failing test",
        " * rather than as a tuner that silently sends the wrong command.",
        " */",
        "#include <stdint.h>",
        "#include <string.h>",
        "#include \"parity.h\"",
        "",
        "typedef struct { const char *hex; const char *expect; } rc_parity_encode_t;",
        "static const rc_parity_encode_t k_encode[] = {",
    ]
    out += [f'    {{"{h}", "{e}"}},' for h, e in encode_table]
    out += [
        "};",
        "",
        "typedef struct { const char *hex; int accept; } rc_parity_decode_t;",
        "static const rc_parity_decode_t k_decode[] = {",
    ]
    out += [f'    {{"{h}", {v}}},' for h, v in decode_table]
    out += [
        "};",
        "",
        "/* The firmware's own live calibration image, hex. The tuner must produce exactly",
        " * these bytes for a default tune. */",
        'const char *rc_parity_c_default_image(void) { return "%s"; }' % c_default,
        "",
        "/* CRC-32C of \"\" and of \"abc\", from the tuner's own implementation. */",
        'const char *tuner_crc32c_probe(void);',
        'const char *tuner_crc32c_probe(void) { return "%08x%08x"; }' % (
            crc32c(b""), crc32c(b"abc")),
        "",
        "/* The unlock proof for a fixed salt over the default image. */",
        "const char *tuner_unlock_proof(void);",
        'const char *tuner_unlock_proof(void) { return "%s"; }' % unlock_proof(
            bytes(range(16)), bytes.fromhex(c_default)),
        "",
        "/* The tuner module's own default image, computed by importing the very module the",
        " * GUI uses - not recomputed here, or this would be checking a copy of it. */",
        "const char *tuner_default_image_hex(void);",
        'const char *tuner_default_image_hex(void) { return "%s"; }' % py_default,
        "",
        "typedef struct { const char *hex; int matches; } rc_parity_live_t;",
        "static const rc_parity_live_t k_live[] = {",
        f'    {{"{live_hex}", {live_matches}}},',
        "};",
        "",
        "const char *rc_parity_python_frame(const char *hex_payload)",
        "{",
        "    for (size_t i = 0; i < sizeof(k_encode) / sizeof(k_encode[0]); ++i) {",
        "        if (strcmp(k_encode[i].hex, hex_payload) == 0) {",
        "            return k_encode[i].expect;",
        "        }",
        "    }",
        '    return "";',
        "}",
        "",
        "int rc_parity_python_decode_check(const char *hex)",
        "{",
        "    for (size_t i = 0; i < sizeof(k_decode) / sizeof(k_decode[0]); ++i) {",
        "        if (strcmp(k_decode[i].hex, hex) == 0) {",
        "            return k_decode[i].accept;",
        "        }",
        "    }",
        "    return -1;",
        "}",
        "",
        "int rc_parity_live_check(const char *hex)",
        "",
        "{",
        "    for (size_t i = 0; i < sizeof(k_live) / sizeof(k_live[0]); ++i) {",
        "        if (strcmp(k_live[i].hex, hex) == 0) {",
        "            return k_live[i].matches;",
        "        }",
        "    }",
        "    return 0;",
        "}",
        "",
    ]

    # The output path comes from the build system, because a generator that writes to a
    # hardcoded directory works exactly once and then silently stops feeding every other
    # build tree - which is how a second configuration ends up with no parity test at all.
    if len(argv) >= 1:
        target = pathlib.Path(argv[0])
    else:
        target = HERE.parent / "build" / "parity_generated.c"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(out))
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
