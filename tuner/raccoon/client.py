"""The tuner's view of an ECU: connect, unlock, read, edit, flash, activate.

This is the layer that decides *when* a calibration is allowed to change. The firmware
enforces every rule again on its own side - the checks here are an early warning and a
better error message, never the control.

Three commitments shape the whole module:

**Nothing is written to a running engine.** ``stage`` fills the inactive slot and leaves the
engine alone; ``verify`` makes the ECU read back and revalidate what it wrote; only
``activate`` swaps. Each step is separate and separately reportable, because a transfer
that half-succeeded must leave a *known* state rather than an assumed one.

**A failure is loud and specific.** Every reply carries a result code and this raises with
the firmware's own name for it. A validation refusal additionally carries the index of the
rule that failed, which is turned into the generated rule's prose - because "rejected"
with no reason gets worked around, and a workaround for a safety rule is how the safety
rule gets removed.

**Cross-item rules are the ECU's job.** This module does not reimplement them.
:mod:`raccoon.calibration` deliberately checks only per-item bounds, and for the same
reason: a second implementation of the 27 cross-item rules would eventually disagree with
the firmware's, and the disagreement would be invisible until the day it mattered. The
refusal comes back over the wire, from the code that actually runs.
"""

from __future__ import annotations

import hashlib
import hmac
import threading
import struct
import time
from dataclasses import dataclass, field

from raccoon import calibration as cal
from raccoon.link import (
    Command,
    Frame,
    FrameParser,
    LiveData,
    Result,
    decode_live,
    safety_text,
)
from raccoon.transport import Timeout, Transport, TransportError

# A bulk read or stage is chunked to this. Comfortably under the ECU's 512-byte frame limit,
# because a reply is the result byte plus the data plus a 4-byte offset: asking for exactly
# the limit produces a reply that cannot be framed, and the ECU drops it silently.
MAX_CHUNK = 480
SALT_SIZE = 16

# GET_INFO body, i.e. the payload AFTER the leading result byte:
#   protocol(4) firmware(4) image_size(4) items(2) cylinders(2) slot(4) version(4)
#   armed(1) rolled_back(1) pad(2) auth_failures(2) salt(16)
# Offsets are into the body, not the whole reply - the firmware writes the result byte at
# index 0, so every field here is one lower than it looks in the C.
_INFO_SALT_OFFSET = 30
_INFO_SIZE = _INFO_SALT_OFFSET + SALT_SIZE


class ClientError(Exception):
    """Anything the operator needs to be told about."""


class ResultError(ClientError):
    """The ECU refused a command."""

    def __init__(self, result: int, message: str = "") -> None:
        self.result = int(result)
        super().__init__(message or result_name(result))


class NotAuthenticated(ClientError):
    """A write was attempted before unlocking."""


def result_name(result: int) -> str:
    try:
        return Result(int(result)).name
    except ValueError:
        return f"RESULT_{int(result)}"


# CRC-32C, Castagnoli, reflected - 0x82F63B78. This is what ``rc_cal_digest`` uses, and it
# is NOT the CRC-32 in zlib or hashlib's crc32. Getting that wrong is invisible: the proof
# still looks like a digest, the client still sends it, and the ECU refuses every unlock
# with no hint as to why. It is therefore pinned against the C by tests/parity.
# The domain separator, so this key can never collide with the same bytes hashed for any
# other reason. It is part of the protocol, not a local convention: the firmware uses the
# same string, and tests/parity checks that it does.
_UNLOCK_DOMAIN = b"RACCOON-UNLOCK-V1"

_CRC32C_POLY = 0x82F63B78
_CRC32C_TABLE: list[int] = []


def _crc32c_table() -> list[int]:
    if not _CRC32C_TABLE:
        table = []
        for i in range(256):
            value = i
            for _ in range(8):
                value = (value >> 1) ^ (_CRC32C_POLY if value & 1 else 0)
            table.append(value)
        _CRC32C_TABLE.extend(table)
    return _CRC32C_TABLE


def crc32c(data: bytes, crc: int = 0) -> int:
    """CRC-32C, matching ``rc_crc32c`` bit for bit."""
    if not data:
        return crc
    table = _crc32c_table()
    value = crc ^ 0xFFFFFFFF
    for byte in data:
        value = table[(value ^ byte) & 0xFF] ^ (value >> 8)
    return value ^ 0xFFFFFFFF


@dataclass
class EcuInfo:
    """Identity from GET_INFO. A datalog whose ECU version is unknown is not evidence."""

    protocol_version: int = 0
    firmware_version: int = 0
    image_size: int = 0
    item_count: int = 0
    cylinder_count: int = 0
    flash_active_slot: int = 0
    flash_active_version: int = 0
    armed: int = 0
    flash_rolled_back: int = 0
    auth_failures: int = 0
    salt: bytes = b""

    @property
    def firmware_text(self) -> str:
        v = self.firmware_version
        return f"{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}"


@dataclass
class LiveFeed:
    """The most recent live frame plus a bounded history for the dashboard."""

    latest: LiveData | None = None
    history: list[LiveData] = field(default_factory=list)
    max_history: int = 900

    def push(self, live: LiveData) -> None:
        self.latest = live
        self.history.append(live)
        if len(self.history) > self.max_history:
            del self.history[: len(self.history) - self.max_history]

    def clear(self) -> None:
        self.latest = None
        self.history.clear()


class EcuClient:
    """One ECU on one link.

    Not safe for concurrent use. The request/reply correlation is a single outstanding
    sequence number; two threads sending at once would pair a reply with the wrong request,
    and the symptom - a log full of values from the wrong moment - is very hard to trace
    back to the cause. The GUI therefore owns one client and does its work on one thread.
    """

    def __init__(self, transport: Transport, timeout: float = 1.0,
                 quiet: float = 0.15) -> None:
        self.transport = transport
        self.timeout = timeout
        # How long the link must be silent before :meth:`latest_live` believes it has seen
        # everything. Longer than one frame interval, or a momentarily slow link looks like
        # the end of the stream and the answer is a frame that has already been superseded.
        self.quiet = quiet
        self.parser = FrameParser()
        self.info: EcuInfo | None = None
        self.live = LiveFeed()
        self.salt: bytes = b""
        self._sequence = 0
        self._image: bytes | None = None
        self._authenticated = False
        self._closed = False
        self._listeners: list = []
        self._busy = threading.Lock()

    # -------------------------------------------------------------- connection

    @property
    def authenticated(self) -> bool:
        return self._authenticated

    @property
    def image(self) -> bytes | None:
        """The last image read from or sent to the ECU. The unlock proof is bound to it."""
        return self._image

    def connect(self) -> EcuInfo:
        self.transport.open()
        self.parser._reset()
        self._authenticated = False
        self._closed = False
        info = self._get_info()
        self.info = info
        self.salt = info.salt
        if info.image_size != cal.IMAGE_SIZE:
            # Refusing here is the single most valuable check in the tuner. Two layouts that
            # differ still have plausible-looking values in most fields, so a mismatch would
            # otherwise show up as a tune that reads fine and runs wrong.
            raise ClientError(
                f"this tuner is built for a {cal.IMAGE_SIZE}-byte calibration and the ECU "
                f"reports {info.image_size}. Refusing to connect: the item offsets would not "
                f"match and every value read or written would be meaningless."
            )
        return info

    def disconnect(self) -> None:
        self.transport.close()
        self._closed = True
        self._authenticated = False
        self.live.clear()

    def __enter__(self) -> EcuClient:
        self.connect()
        return self

    def __exit__(self, *_exc) -> None:
        self.disconnect()

    def add_listener(self, callback) -> None:
        """Register ``callback(kind, payload)`` for unsolicited frames.

        An observer that raises is dropped rather than allowed to kill the read loop: a
        stale gauge is a better outcome than an ECU that appears to have disconnected.
        """
        self._listeners.append(callback)

    def _notify(self, kind: str, payload) -> None:
        for callback in list(self._listeners):
            try:
                callback(kind, payload)
            except Exception:
                pass

    # ---------------------------------------------------------------- plumbing

    def _next_sequence(self) -> int:
        # Allowed to wrap to match the 16-bit wire field. A link that has sent 65535
        # requests will have been reopened long before a wrap could collide with a
        # request still in flight.
        self._sequence = (self._sequence + 1) & 0xFFFF
        return self._sequence

    def _request(self, command: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        if self._closed:
            raise TransportError("the link is closed")
        # One caller at a time, enforced rather than documented.
        #
        # Two threads sharing a client interleave their reads, so each can consume the
        # other's reply and each times out on a link that is working. The resulting failure
        # points at the network and at nothing else, and it appears only under load. A
        # second caller is far more likely to be a design mistake than a real requirement,
        # so it is refused where it can be found: here, with the other thread named.
        if not self._busy.acquire(blocking=False):
            raise ClientError(
                "this EcuClient is already in use by another thread. One client belongs to "
                "one thread; give each its own, or marshal the work."
            )
        try:
            return self._request_locked(command, payload, timeout)
        finally:
            self._busy.release()

    def _request_locked(self, command: int, payload: bytes, timeout: float | None) -> bytes:
        wait = self.timeout if timeout is None else timeout
        sequence = self._next_sequence()
        self.transport.write(Frame(command=command, sequence=sequence, payload=payload).encode(),
                             timeout=wait)
        # The command travels with the wait so a refusal can be described in terms of what
        # was actually refused. "Rejected" on its own is not something an operator can act
        # on, and an un-actionable error message is one they learn to work around.
        body = self._await_response(sequence, wait, command)
        return body

    def _await_response(self, sequence: int, timeout: float, command: int = 0) -> bytes:
        """Read until the reply to ``sequence`` arrives, dispatching everything else.

        Live data shares the wire with replies, so this filters rather than reads once. A
        reply to a request already abandoned is dropped: the operation it answers either
        happened or did not, and acting on it now would be acting on a stale answer.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TransportError(
                    f"no response within {timeout:.2f}s - the ECU may be busy, asleep, or "
                    f"the link may be down"
                )
            # Bounded per read, not given the whole remaining budget.
            #
            # A live-data stream shares the wire with responses. Handing each frame read the
            # entire remaining timeout means every unsolicited frame spends the deadline that
            # the REPLY is waiting on - so a bulk read, which is many round trips, exhausts
            # its budget purely because the ECU is also logging. The symptom is an
            # intermittent "nothing arrived within 0.1s" on a link that is working perfectly,
            # and it gets worse the faster the log rate. A short per-read timeout with the
            # overall deadline checked separately handles both cases.
            frame = self._read_one(min(remaining, 0.25))
            if frame.command == Command.RESPONSE:
                if frame.sequence != sequence:
                    continue
                if not frame.payload:
                    raise TransportError("the ECU sent an empty response")
                result = frame.payload[0]
                if result != Result.OK:
                    raise ResultError(result, _describe(command, result, frame.payload[1:]))
                return frame.payload[1:]
            if frame.command == Command.GET_LIVE:
                live = decode_live(frame.payload)
                if live is not None:
                    self.live.push(live)
                    self._notify("live", live)
                continue
            self._notify("frame", frame)

    def _read_one(self, timeout: float) -> Frame:
        while True:
            # Whatever has arrived, not an exact count. A live-data frame is ~100 bytes, so
            # asking a stream for 256 blocks until 156 more arrive - which on a link that is
            # only ever sent them is never, and the reply the caller is waiting for sits
            # unread in the same buffer the whole time.
            chunk = self.transport.read_available(512, timeout=timeout)
            frames = self.parser.feed(chunk)
            if frames:
                return frames[0]

    def _require_auth(self) -> None:
        if not self._authenticated:
            raise NotAuthenticated(
                "unlock the ECU first. The ECU would refuse the write anyway; this saves "
                "the round trip and tells you why."
            )

    # ------------------------------------------------------------------- reads

    def _get_info(self) -> EcuInfo:
        body = self._request(Command.GET_INFO)
        if len(body) < _INFO_SIZE:
            raise TransportError(
                f"the ECU sent a {len(body)}-byte info block; expected {_INFO_SIZE}"
            )
        return EcuInfo(
            protocol_version=unpack_u32(body, 0),
            firmware_version=unpack_u32(body, 4),
            image_size=unpack_u32(body, 8),
            item_count=unpack_u16(body, 12),
            cylinder_count=unpack_u16(body, 14),
            flash_active_slot=unpack_u32(body, 16),
            flash_active_version=unpack_u32(body, 20),
            armed=body[24],
            flash_rolled_back=body[25],
            auth_failures=unpack_u16(body, 29),
            salt=bytes(body[_INFO_SALT_OFFSET:_INFO_SALT_OFFSET + SALT_SIZE]),
        )

    def refresh_info(self) -> EcuInfo:
        self.info = self._get_info()
        return self.info

    def read_bulk(self, on_progress=None) -> cal.Calibration:
        """Read the ECU's live calibration image, in chunks.

        Every reply echoes the offset it is answering. The client checks it, because a reply
        filed at the wrong offset would produce an image that is the right size, passes a
        CRC, and is a mixture of two tunes - the one corruption no checksum would catch.
        """
        info = self.info or self.refresh_info()
        total = info.image_size
        blob = bytearray()
        offset = 0
        while offset < total:
            want = min(MAX_CHUNK, total - offset)
            body = self._request(Command.READ_BULK, pack_u32(offset) + pack_u16(want))
            if len(body) < 4:
                raise TransportError(f"bulk read at {offset} returned a {len(body)}-byte header")
            got = unpack_u32(body, 0)
            if got != offset:
                raise TransportError(
                    f"asked for offset {offset}, the ECU answered with data from {got}. "
                    f"Refusing to file it at the wrong place."
                )
            chunk = body[4:]
            blob.extend(chunk)
            offset += len(chunk)
            if on_progress is not None:
                on_progress(offset, total)
        if len(blob) != total:
            raise TransportError(f"read {len(blob)} bytes but the ECU reports {total}")
        self._image = bytes(blob)
        return cal.Calibration.from_bytes(self._image)

    def read_item(self, item_id: int, index: int | None = None) -> float:
        payload = pack_u16(item_id) + (pack_u16(index) if index is not None else b"")
        body = self._request(Command.READ_ITEM, payload)
        if len(body) < 4:
            raise TransportError("short item reply")
        return unpack_f32(body, 0)

    def get_faults(self) -> int:
        return pack_u32(self._request(Command.GET_FAULTS), 0)

    def get_status(self) -> dict:
        body = self._request(Command.GET_STATUS)
        if len(body) < 27:
            raise TransportError("short status reply")
        return {
            "active_slot": body[1],
            "rolled_back": body[2],
            "armed": body[3],
            "authenticated": body[4],
            "dirty": unpack_u32(body, 5) != 0,
            "staged_bytes": unpack_u32(body, 9),
            "version_a": unpack_u32(body, 13),
            "version_b": unpack_u32(body, 17),
            "auth_failures": unpack_u32(body, 21),
            "logging_hz": unpack_u16(body, 25),
        }

    # ------------------------------------------------------------ authentication

    def proof(self, code: str, image: bytes | None = None) -> bytes:
        """The unlock proof for ``code``, as ``rc_link_service.c`` computes it.

            key   = SHA-256("RACCOON-UNLOCK-V1" || salt || code)
            proof = HMAC-SHA256(key, salt || le32(CRC32C(image)))

        The code never crosses the wire. Deriving the key from it as well as from the salt is
        what makes this a lock: the salt is published in GET_INFO, so a proof made from the
        salt alone would authenticate anyone who has read the identity block - which is
        every client that ever connects. The image digest means a captured proof stops
        working as soon as a different tune is loaded, so it cannot be saved and replayed
        into an arbitrary calibration later.
        """
        if not self.salt:
            raise ClientError("no salt from the ECU; connect first")
        if not code:
            raise ClientError("an empty unlock code")
        blob = image if image is not None else self._image
        if blob is None:
            blob = cal.default_image()
        key = hashlib.sha256(_UNLOCK_DOMAIN + self.salt + code.encode("utf-8")).digest()
        material = self.salt + crc32c(blob).to_bytes(4, "little")
        return hmac.new(key, material, hashlib.sha256).digest()

    def authenticate(self, code: str, image: bytes | None = None) -> None:
        if not code:
            raise ClientError("an empty unlock code")
        if not self.salt:
            raise ClientError("connect before unlocking: the salt comes from the ECU")
        try:
            self._authenticate_once(code, image)
        except ResultError:
            if image is not None:
                # The caller named the image, so its contents are not in question.
                raise
            # The proof is bound to the tune the ECU is RUNNING, and ours may be out of date
            # - a previous session flashed something, or the tune was changed on the bench.
            # Refusing to unlock then is correct, but leaving the operator with "locked" and
            # no way forward is not: read what is actually loaded, build the proof for that,
            # and try once more. One retry, on a fresh read, so a wrong code still fails.
            self.read_bulk()
            self._authenticate_once(code, None)
        self._authenticated = True

    def _authenticate_once(self, code: str, image: bytes | None) -> None:
        proof = self.proof(code, image)
        self._request(Command.AUTHENTICATE, self.salt + proof, timeout=max(self.timeout, 5.0))

    # ------------------------------------------------------------------ writes

    def write_item(self, item, value) -> None:
        """Write one scalar or table cell into the ECU's working image.

        Sent as the *engineering* value, because that is what ``rc_cal_set_float`` and
        ``rc_cal_set_table_cell`` take on the far side and they do the scaling. Sending the
        stored form instead would apply the item's scale twice, and for a table with a 0.1
        factor that is a ten-fold fuelling error - which passes a range check and runs.
        """
        self._require_auth()
        if isinstance(value, (list, tuple)):
            # A list means "every cell of this multi-cell item", and the ECU's protocol
            # needs to be told WHICH of them it is. Dispatching on the item's own kind
            # rather than on a caller flag means a caller that passes an axis' breakpoints
            # cannot accidentally write them over a table.
            write = self.write_axis_point if item.is_axis else self.write_table_cell
            for index, cell in enumerate(value):
                write(item, index, cell)
            return
        self._request(Command.WRITE_ITEM, pack_u16(item.id) + pack_f32(float(value)))

    def write_table_cell(self, item, index: int, value: float) -> None:
        self._require_auth()
        self._request(
            Command.WRITE_ITEM,
            pack_u16(item.id) + pack_f32(float(value)) + pack_u16(index) + b"\x01",
        )

    def write_axis_point(self, item, index: int, value: float) -> None:
        """Move one breakpoint of an axis.

        Target 2 in the protocol's own words, rather than letting the ECU work it out from
        the item type: a client that guessed wrong here would move a table cell when the
        operator dragged a breakpoint, and that is the kind of change that gets flashed
        without anyone noticing which one it was.
        """
        self._require_auth()
        self._request(
            Command.WRITE_ITEM,
            pack_u16(item.id) + pack_f32(float(value)) + pack_u16(index) + b"\x02",
        )

    # ------------------------------------------------------------------- flash

    def stage(self, image: bytes, on_progress=None) -> None:
        """Send an image into the inactive slot. The engine is unaffected.

        Sending the image the client already has, rather than the working image the ECU has
        been accumulating item by item, means the two can never disagree. The ECU validates
        the assembled image on VERIFY, so a client that drifted is caught there.
        """
        self._require_auth()
        total = len(image)
        offset = 0
        while offset < total:
            count = min(MAX_CHUNK, total - offset)
            self._request(
                Command.STAGE,
                pack_u32(offset) + pack_u16(count) + image[offset : offset + count],
                timeout=max(self.timeout, 2.0),
            )
            offset += count
            if on_progress is not None:
                on_progress(offset, total)

    def verify_staged(self) -> int:
        """Ask the ECU to read the staged slot back and revalidate it. Returns its CRC32."""
        self._require_auth()
        body = self._request(Command.VERIFY, timeout=max(self.timeout, 5.0))
        if len(body) < 4:
            raise TransportError("short verify reply")
        return unpack_u32(body, 0)

    def activate(self) -> None:
        self._require_auth()
        self._request(Command.ACTIVATE, timeout=max(self.timeout, 5.0))
        self.refresh_info()

    def rollback(self) -> None:
        self._require_auth()
        self._request(Command.ROLLBACK, timeout=max(self.timeout, 5.0))
        self.refresh_info()

    def flash_calibration(self, edited: cal.Calibration, code: str, on_progress=None) -> None:
        """The whole change sequence, with the image verified before it is activated.

        The ECU is the authority on whether an image is safe - it revalidates on VERIFY and
        again on ACTIVATE. This sequence exists so that a failure at any step leaves a known
        state: the running tune is untouched until the last command, and that command only
        runs once the ECU has read back and agreed with what was sent.
        """
        image = edited.to_bytes()
        if len(image) != cal.IMAGE_SIZE:
            raise ClientError(f"the image is {len(image)} bytes; expected {cal.IMAGE_SIZE}")
        if not self._authenticated:
            # Re-unlocking binds a fresh proof to this image, so the proof cannot be reused
            # against a different one later.
            self.authenticate(code, image=image)
        self.stage(image, on_progress=on_progress)
        digest = self.verify_staged()
        expected = crc32c(image)
        if digest != expected:
            raise ClientError(
                f"the ECU read back a different image than was sent (expected CRC "
                f"0x{expected:08X}, got 0x{digest:08X}). Not activating."
            )
        self.activate()
        self._image = image

    # ------------------------------------------------------------------ logging

    def set_logging(self, rate_hz: int) -> None:
        self._request(Command.SET_LOGGING, pack_u16(max(0, min(rate_hz, 0xFFFF))))

    def poll_live(self, timeout: float = 0.0) -> LiveData | None:
        """One non-blocking look at the live stream, or None.

        For a paint timer. ``timeout`` of 0 means "whatever has already arrived", so the
        caller never waits and a dead link costs nothing - which is the whole point of
        polling rather than asking for a frame.
        """
        newest: LiveData | None = None
        try:
            frame = self._read_one(timeout)
        except (TransportError, Timeout):
            return None
        if frame.command == Command.GET_LIVE:
            live = decode_live(frame.payload)
            if live is not None:
                self.live.push(live)
                self._notify("live", live)
                newest = live
        return newest

    def drain_live(self, budget_seconds: float = 0.0) -> LiveData | None:
        """Consume whatever live data has arrived, without waiting for a frame.

        Returns the newest frame, or None. The dashboard calls this from its paint timer so
        a dead link degrades to a stale gauge rather than a frozen, confident-looking UI.
        """
        newest: LiveData | None = None
        deadline = time.monotonic() + max(budget_seconds, 0.0)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 and newest is not None:
                break
            try:
                frame = self._read_one(max(remaining, 0.001))
            except TransportError:
                break
            if frame.command == Command.GET_LIVE:
                live = decode_live(frame.payload)
                if live is not None:
                    self.live.push(live)
                    self._notify("live", live)
                    newest = live
        return newest

    def wait_live(self, timeout: float = 2.0) -> LiveData:
        """Block until one live frame arrives. Raises rather than returning a blank one."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self._read_one(max(deadline - time.monotonic(), 0.001))
            if frame.command == Command.GET_LIVE:
                live = decode_live(frame.payload)
                if live is not None:
                    self.live.push(live)
                    return live
        raise TransportError(f"no live data within {timeout:.2f}s - is logging enabled?")

    def latest_live(self, budget: float = 1.0) -> LiveData | None:
        """The newest frame the link has produced, not merely the next one queued.

        Not the same as :meth:`wait_live`, and the difference matters whenever the caller
        asks a question about the present. On a streaming link the socket holds every frame
        sent since the last read, so the FIRST one available can be seconds old - a caller
        that stopped the engine and read again would be looking at the engine from before it
        stopped, and conclude the stop did not work.

        So this drains: every frame already queued is read and discarded, keeping only the
        last, and it stops when the link goes quiet for `quiet` seconds. `budget` is a ceiling
        so a link that is saturating cannot keep this caller waiting forever.

        Returns None - not the last frame seen - when nothing arrived. Those are different
        situations and a caller polling for a state change has to be able to tell them: a
        silent link that returned its previous frame reports the OLD state as if it were
        current, and a caller waiting for the engine to stop is then told, ten times a
        second, that the engine has not stopped. Callers that want the history ask for
        :attr:`LiveFeed.latest`.
        """
        newest: LiveData | None = None
        deadline = time.monotonic() + budget
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                frame = self._read_one(min(remaining, self.quiet))
            except TransportError:
                # Nothing more is coming: whatever arrived is as new as this link gets.
                break
            if frame.command == Command.GET_LIVE:
                live = decode_live(frame.payload)
                if live is not None:
                    self.live.push(live)
                    newest = live
        return newest


# --------------------------------------------------------------------- helpers


def _describe(command: int, result: int, body: bytes) -> str:
    """A refusal in words an operator can act on."""
    name = _command_name(command)
    text = f"{name} refused: {result_name(result)}"
    if result == Result.RULE_VIOLATION and len(body) >= 2:
        index = body[0] | (body[1] << 8)
        rule = cal.rule_text(index)
        if rule:
            text = f"{name} refused: the tune breaks a rule - {rule}"
    elif result == Result.OUT_OF_RANGE:
        text = f"{name} refused: the value is outside the range that item allows"
    elif result == Result.NOT_ARMED:
        text = f"{name} refused: the engine is running. Stop it before changing a tune."
    elif result == Result.NOT_AUTHENTICATED:
        text = f"{name} refused: the ECU is locked. Enter the unlock code first."
    elif result == Result.STORAGE:
        text = f"{name} refused: the flash layer would not accept it"
    return text


def _command_name(command: int) -> str:
    try:
        return Command(int(command)).name
    except ValueError:
        return f"command 0x{int(command):02X}"


# Bytes in, bytes out. Packing and unpacking are named apart on purpose: the two are one
# character different in use and produce completely different numbers when confused, and
# `pack_u32(n)` reading as "the 32-bit value at n" is a mistake that type checking cannot
# catch because both are ints.

def pack_u32(value: int) -> bytes:
    return int(value).to_bytes(4, "little", signed=False)


def pack_u16(value: int) -> bytes:
    return int(value).to_bytes(2, "little", signed=False)


def pack_f32(value: float) -> bytes:
    return struct.pack("<f", float(value))


def unpack_u32(blob: bytes, offset: int = 0) -> int:
    return int.from_bytes(blob[offset : offset + 4], "little", signed=False)


def unpack_u16(blob: bytes, offset: int = 0) -> int:
    return int.from_bytes(blob[offset : offset + 2], "little", signed=False)


def unpack_f32(blob: bytes, offset: int = 0) -> float:
    return struct.unpack_from("<f", blob, offset)[0]
