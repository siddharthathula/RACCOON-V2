"""RACCOON V2 - tuner link, Python side.

This is the exact counterpart of firmware/src/link/rc_link.c. Every constant, offset and
checksum is written out rather than imported, and the two are checked against each other by
tests/test_link_parity.py, which drives both implementations over the same bytes. Two
implementations of a wire format that are merely believed to agree is how a tuner ends up
sending ACTIVATE into a length field the ECU reads as something else entirely.
"""

from __future__ import annotations

import enum
import pathlib
import struct
import zlib
from dataclasses import dataclass, field

SOF = 0xA5
VERSION = 1
HEADER_SIZE = 8
MAX_PAYLOAD = 512
MAX_FRAME = HEADER_SIZE + MAX_PAYLOAD + 4

# CRC-32/ISO-HDLC. zlib.crc32 is the same function, including the initial and final
# conditioning, which is why the C side uses a raw running value and XORs once at the end.
def crc32(data: bytes, seed: int = 0) -> int:
    return zlib.crc32(data, seed) & 0xFFFFFFFF


class Command(enum.IntEnum):
    PING = 0x01
    PONG = 0x02
    GET_INFO = 0x10
    READ_ITEM = 0x11
    WRITE_ITEM = 0x12
    READ_BULK = 0x13
    STAGE = 0x14
    VERIFY = 0x15
    ACTIVATE = 0x16
    ROLLBACK = 0x17
    GET_STATUS = 0x20
    GET_LIVE = 0x21
    GET_FAULTS = 0x22
    SET_LOGGING = 0x23
    AUTHENTICATE = 0x30
    RESPONSE = 0x7F


class Result(enum.IntEnum):
    OK = 0
    BAD_CHECKSUM = 1
    BAD_COMMAND = 2
    BAD_LENGTH = 3
    BAD_ID = 4
    OUT_OF_RANGE = 5
    RULE_VIOLATION = 6
    NOT_AUTHENTICATED = 7
    NOT_ARMED = 8
    BUSY = 9
    STORAGE = 10
    INTERNAL = 11


RESULT_TEXT = {
    Result.OK: "ok",
    Result.BAD_CHECKSUM: "the frame failed its checksum",
    Result.BAD_COMMAND: "unrecognised command",
    Result.BAD_LENGTH: "wrong payload length",
    Result.BAD_ID: "no such calibration item",
    Result.OUT_OF_RANGE: "the value is outside that item's range",
    Result.RULE_VIOLATION: "the value breaks a calibration rule",
    Result.NOT_AUTHENTICATED: "not authenticated",
    Result.NOT_ARMED: "refused: the engine is running",
    Result.BUSY: "busy",
    Result.STORAGE: "the storage operation failed",
    Result.INTERNAL: "internal error",
}


@dataclass
class Frame:
    command: int
    sequence: int = 0
    flags: int = 0
    payload: bytes = b""
    version: int = VERSION

    def encode(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("payload exceeds the protocol limit")
        head = struct.pack(
            "<BBBBHH", SOF, self.version, self.command & 0xFF, self.flags & 0xFF,
            self.sequence & 0xFFFF, len(self.payload),
        )
        body = head + self.payload
        return body + struct.pack("<I", crc32(body))

    @classmethod
    def decode(cls, raw: bytes) -> "Frame":
        if len(raw) < HEADER_SIZE + 4:
            raise ValueError("frame too short")
        sof, version, command, flags, sequence, length = struct.unpack_from("<BBBBHH", raw, 0)
        if sof != SOF:
            raise ValueError("bad start of frame")
        if len(raw) < HEADER_SIZE + length + 4:
            raise ValueError("frame truncated")
        body = raw[: HEADER_SIZE + length]
        (checksum,) = struct.unpack_from("<I", raw, HEADER_SIZE + length)
        if checksum != crc32(body):
            raise ValueError("checksum mismatch")
        return cls(
            command=command, sequence=sequence, flags=flags,
            payload=raw[HEADER_SIZE: HEADER_SIZE + length], version=version,
        )


class FrameParser:
    """Byte-oriented resynchronising parser, mirroring the C one.

    A length field is controlled by whoever is on the other end, so a frame that declares
    more bytes than it delivers must not hold the parser open forever. The timeout is the
    only honest signal that the rest is never coming; resynchronising on every SOF is not
    available, because 0xA5 turns up inside payloads.
    """

    FRAME_TIMEOUT_US = 250_000

    def __init__(self) -> None:
        self._buf = bytearray()
        self.frames_ok = 0
        self.frames_bad_crc = 0
        self.frames_dropped = 0
        self._started_us: int | None = None

    def _reset(self) -> None:
        self._buf.clear()
        self._started_us = None

    def tick(self, now_us: int) -> None:
        if not self._buf or self._started_us is None:
            return
        if now_us - self._started_us > self.FRAME_TIMEOUT_US:
            self.frames_dropped += 1
            self._reset()

    def feed(self, data: bytes, now_us: int = 0) -> list[Frame]:
        self.tick(now_us)
        out: list[Frame] = []
        for byte in data:
            if not self._buf:
                if byte == SOF:
                    self._buf.append(byte)
                    self._started_us = now_us
                continue
            self._buf.append(byte)
            if len(self._buf) < HEADER_SIZE:
                continue
            length = struct.unpack_from("<H", self._buf, 6)[0]
            if length > MAX_PAYLOAD:
                self.frames_dropped += 1
                self._reset()
                continue
            if len(self._buf) < HEADER_SIZE + length + 4:
                continue
            raw = bytes(self._buf)
            self._reset()
            try:
                out.append(Frame.decode(raw))
            except ValueError:
                self.frames_bad_crc += 1
        return out


# --------------------------------------------------------------- live data

def _max_from_header(name: str, fallback: int) -> int:
    """Read an RC_MAX_* constant out of the firmware header.

    These are not numbers to be copied. A copy that is one element short produces a struct
    that still packs, still unpacks, and is just 4 or 32 bytes smaller than the one on the
    wire - so every field after the mismatch decodes as a different field's value, with no
    error anywhere. The parity test would catch it, but only if it is built; reading the
    header means the Python side cannot be wrong in the first place.
    """
    header = pathlib.Path(__file__).resolve().parents[2] / "firmware" / "include" / "rc" / "rc_types.h"
    try:
        for line in header.read_text().splitlines():
            if line.startswith(f"#define {name} "):
                return int(line.split()[2].rstrip("uU"), 0)
    except OSError:
        pass
    return fallback


MAX_LAMBDA = _max_from_header("RC_MAX_LAMBDA", 4)
MAX_IGNITION = _max_from_header("RC_MAX_IGNITION", 8)
MAX_INJECTORS = _max_from_header("RC_MAX_INJECTORS", 8)

# Laid out in exactly the order rc_link_encode_live writes them.
LIVE_STRUCT = struct.Struct(
    "<II"                                   # sequence, engine time
    + "f" * 5                               # rpm, map, tps, iat, ect
    + "f" * MAX_LAMBDA                       # lambda
    + "BBB"                                  # load source, sync state, confidence
    + "B" * MAX_LAMBDA                       # lambda valid
    + "f" * MAX_IGNITION                     # ignition degrees
    + "f" * MAX_INJECTORS                     # injector ms
    + "f" * MAX_IGNITION                     # dwell ms
    + "ffff"                                 # battery, fuel pressure, oil pressure, oil temp
    + "B" * MAX_IGNITION                     # ignition cut
    + "II"                                   # safety active, arb winner
    + "ff"                                   # torque allowance, safety factor
    + "B"                                    # armed
)
LIVE_SIZE = LIVE_STRUCT.size


@dataclass
class LiveData:
    sequence: int = 0
    engine_time_us: int = 0
    rpm: float = 0.0
    map_kpa: float = 0.0
    tps_pct: float = 0.0
    iat_c: float = 0.0
    ect_c: float = 0.0
    # Named lambda_values, not lambda: `lambda` is a Python keyword, and a field
    # called after it cannot be written down at all.
    lambda_values: list[float] = field(default_factory=lambda: [0.0] * MAX_LAMBDA)
    load_source: int = 0
    sync_state: int = 0
    sync_confidence: int = 0
    lambda_valid: list[int] = field(default_factory=lambda: [0] * MAX_LAMBDA)
    ignition_deg: list[float] = field(default_factory=lambda: [0.0] * MAX_IGNITION)
    injector_ms: list[float] = field(default_factory=lambda: [0.0] * MAX_INJECTORS)
    dwell_ms: list[float] = field(default_factory=lambda: [0.0] * MAX_IGNITION)
    battery_v: float = 0.0
    fuel_pressure_kpa: float = 0.0
    oil_pressure_bar: float = 0.0
    oil_temp_c: float = 0.0
    ignition_cut: list[int] = field(default_factory=lambda: [0] * MAX_IGNITION)
    safety_active: int = 0
    torque_arb_winner: int = 0
    torque_allowed: float = 1.0
    safety_factor: float = 1.0
    armed: int = 0


SYNC_NAMES = {
    0: "no signal", 1: "initialising", 2: "seeking sync", 3: "candidate",
    4: "synced", 5: "recovering", 6: "fault",
}
SAFETY_NAMES = {
    0x0001: "overspeed", 0x0002: "lean", 0x0004: "rich", 0x0008: "knock",
    0x0010: "oil pressure", 0x0020: "oil temperature", 0x0040: "ect",
    0x0080: "battery", 0x0100: "sync lost", 0x0200: "crank only",
    0x0400: "sensor", 0x8000: "launch",
}


def decode_live(payload: bytes) -> LiveData | None:
    """Decode a live frame, or None if it is not complete.

    None rather than a half-populated object: a log that cannot distinguish "the engine
    reported zeros" from "the frame was truncated" is not evidence of anything.
    """
    if len(payload) < LIVE_SIZE:
        return None
    values = LIVE_STRUCT.unpack_from(payload, 0)
    idx = 0

    def take():
        nonlocal idx
        v = values[idx]
        idx += 1
        return v

    live = LiveData()
    live.sequence = take()
    live.engine_time_us = take()
    live.rpm = take()
    live.map_kpa = take()
    live.tps_pct = take()
    live.iat_c = take()
    live.ect_c = take()
    live.lambda_values = [take() for _ in range(MAX_LAMBDA)]
    live.load_source = take()
    live.sync_state = take()
    live.sync_confidence = take()
    live.lambda_valid = [take() for _ in range(MAX_LAMBDA)]
    live.ignition_deg = [take() for _ in range(MAX_IGNITION)]
    live.injector_ms = [take() for _ in range(MAX_INJECTORS)]
    live.dwell_ms = [take() for _ in range(MAX_IGNITION)]
    live.battery_v = take()
    live.fuel_pressure_kpa = take()
    live.oil_pressure_bar = take()
    live.oil_temp_c = take()
    live.ignition_cut = [take() for _ in range(MAX_IGNITION)]
    live.safety_active = take()
    live.torque_arb_winner = take()
    live.torque_allowed = take()
    live.safety_factor = take()
    live.armed = take()
    return live


def safety_text(mask: int) -> str:
    if mask == 0:
        return "none"
    names = [name for bit, name in SAFETY_NAMES.items() if mask & bit]
    return ", ".join(names) if names else f"0x{mask:04x}"
