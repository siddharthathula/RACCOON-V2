"""Byte transports for the tuner link.

The link protocol in :mod:`raccoon.link` is deliberately transport-agnostic - the same
frames travel over USB CDC on the bench, CAN to the ESP32-S3, and TCP over WiFi. This
module is the only place that knows how bytes actually move, so a framing bug cannot be
fixed in one path and left broken in another.

Three rules hold for every transport here:

* :meth:`Transport.write` either writes every byte or raises. A partial write is not a
  short frame to be recovered from, it is a frame the ECU will now wait to finish
  receiving while the operator assumes it arrived.
* :meth:`Transport.read` raises on timeout rather than returning less. A caller that
  silently accepts a short read ends up unpacking a half-frame into plausible-looking
  numbers.
* Nothing here interprets a byte. Framing and checksum belong to :mod:`raccoon.link`.

:func:`~raccoon.link.read_available` is the method a *stream parser* wants, and the
distinction is not academic. :meth:`Transport.read` blocks until it has exactly the number
of bytes asked for, which is the right contract for reading a length-prefixed structure and
exactly wrong for reading frames: a live-data frame is about 100 bytes, so a parser asking
for 256 sits in ``recv`` for the other 156 forever, holding up the very reply the client is
waiting for. The symptom is a link that answers everything except the one command issued
while a log stream is running.
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Protocol

DEFAULT_TIMEOUT = 1.0


class TransportError(Exception):
    """The link failed in a way the operator needs to see."""


class Timeout(TransportError):
    """A read or write did not complete in time.

    Separate from :class:`TransportError` because the recovery differs: a timeout usually
    means the ECU is busy or asleep and is worth retrying, whereas a broken pipe needs the
    link reopened.
    """


class Transport(Protocol):
    """A bidirectional byte pipe."""

    def open(self) -> None: ...

    def close(self) -> None: ...

    def write(self, data: bytes, timeout: float = DEFAULT_TIMEOUT) -> None: ...

    def read(self, count: int, timeout: float = DEFAULT_TIMEOUT) -> bytes: ...

    def read_available(self, limit: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        """Return up to ``limit`` bytes, blocking only until at least one arrives."""

    def describe(self) -> str: ...


class TcpTransport:
    """A blocking TCP transport, used for WiFi and for the simulator.

    ``nodelay`` is on. Nagle's algorithm delays a small write until the next one arrives,
    which on a request/response protocol turns a 1 ms round trip into a delayed-ACK stall
    of tens of milliseconds - and with a live-data frame in the opposite direction, it
    produces irregular delivery that is very hard to tell apart from a bad link.
    """

    def __init__(self, host: str, port: int, timeout: float = DEFAULT_TIMEOUT) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None

    def open(self) -> None:
        if self._sock is not None:
            return
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as exc:
            raise TransportError(f"cannot reach {self.describe()}: {exc}") from exc
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(self.timeout)
        self._sock = sock

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None

    def _require(self) -> socket.socket:
        if self._sock is None:
            raise TransportError("the link is not open")
        return self._sock

    def write(self, data: bytes, timeout: float = DEFAULT_TIMEOUT) -> None:
        sock = self._require()
        sock.settimeout(timeout)
        try:
            # sendall returns None. It either writes everything or raises, so there is no
            # count to check - and comparing its None against a length would reject every
            # frame on a link that is working perfectly.
            sock.sendall(data)
        except socket.timeout as exc:
            raise Timeout(f"write timed out after {timeout:.2f}s") from exc
        except OSError as exc:
            raise TransportError(f"write failed: {exc}") from exc

    def read_available(self, limit: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        sock = self._require()
        sock.settimeout(timeout)
        try:
            chunk = sock.recv(limit)
        except socket.timeout as exc:
            raise Timeout(f"read timed out after {timeout:.2f}s") from exc
        except OSError as exc:
            raise TransportError(f"read failed: {exc}") from exc
        if not chunk:
            raise TransportError("the ECU closed the link")
        return chunk

    def read(self, count: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        """Read exactly ``count`` bytes.

        A stream socket returns whatever happens to have arrived, so this loops. It also
        caps the wait per chunk rather than for the whole read, so a link delivering a slow
        dribble is still subject to the overall deadline.
        """
        sock = self._require()
        sock.settimeout(timeout)
        chunks: list[bytes] = []
        got = 0
        while got < count:
            try:
                chunk = sock.recv(count - got)
            except socket.timeout as exc:
                raise Timeout(f"read timed out after {timeout:.2f}s ({got}/{count} bytes)") from exc
            except OSError as exc:
                raise TransportError(f"read failed: {exc}") from exc
            if not chunk:
                raise TransportError(
                    f"the ECU closed the link with {got} of {count} bytes outstanding"
                )
            chunks.append(chunk)
            got += len(chunk)
        return b"".join(chunks)

    def describe(self) -> str:
        return f"{self.host}:{self.port}"


class LoopbackTransport:
    """An in-process transport that hands bytes to a callable.

    This is what lets the GUI, the client, and the whole flash workflow be driven in a test
    or against a simulator with no serial port, no WiFi, and no ECU. The same
    :class:`Transport` interface is used, so a code path that works here works over a real
    link - and, more usefully, a failure raised here is a failure of the tuner's logic
    rather than of the bench.
    """

    def __init__(self, exchange, name: str = "loopback") -> None:
        # exchange(request: bytes, timeout: float) -> bytes
        self._exchange = exchange
        self._name = name
        self._pending = b""
        self._lock = threading.Lock()

    def open(self) -> None:
        return

    def close(self) -> None:
        self._pending = b""

    def write(self, data: bytes, timeout: float = DEFAULT_TIMEOUT) -> None:
        with self._lock:
            try:
                self._pending += self._exchange(data, timeout)
            except Exception as exc:  # surfaced verbatim; the caller knows the protocol
                raise TransportError(str(exc)) from exc

    def read_available(self, limit: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        with self._lock:
            if not self._pending:
                raise Timeout("loopback has nothing available")
            out, self._pending = self._pending[:limit], self._pending[limit:]
            return out

    def read(self, count: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        with self._lock:
            if len(self._pending) < count:
                raise Timeout(f"loopback has {len(self._pending)} of {count} bytes")
            out, self._pending = self._pending[:count], self._pending[count:]
            return out

    def describe(self) -> str:
        return self._name


class SerialTransport:
    """USB CDC / Bluetooth serial.

    ``pyserial`` is imported lazily so the rest of the tuner - and every test that does not
    open a port - works on a machine that has never had pyserial installed.
    """

    def __init__(self, port: str, baud: int = 500000, timeout: float = DEFAULT_TIMEOUT) -> None:
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self._port = None

    def open(self) -> None:
        if self._port is not None:
            return
        try:
            import serial  # noqa: PLC0415 - deliberately deferred
        except ImportError as exc:
            raise TransportError(
                "pyserial is required for a serial link: pip install pyserial"
            ) from exc
        try:
            self._port = serial.Serial(
                self.port, self.baud, timeout=self.timeout, write_timeout=self.timeout
            )
        except Exception as exc:
            raise TransportError(f"cannot open {self.port}: {exc}") from exc
        # The board may have been reset by opening the port, so anything half-received from
        # a previous session is not part of this one.
        self._port.reset_input_buffer()

    def close(self) -> None:
        if self._port is not None:
            self._port.close()
            self._port = None

    def _require(self):
        if self._port is None:
            raise TransportError("the link is not open")
        return self._port

    def write(self, data: bytes, timeout: float = DEFAULT_TIMEOUT) -> None:
        port = self._require()
        port.write_timeout = self.timeout if timeout is None else timeout
        try:
            written = port.write(data)
        except Exception as exc:
            raise TransportError(f"write failed: {exc}") from exc
        if written != len(data):
            raise TransportError(f"short write: {written} of {len(data)} bytes")

    def read_available(self, limit: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        port = self._require()
        port.timeout = self.timeout if timeout is None else timeout
        try:
            chunk = port.read(limit)
        except Exception as exc:
            raise TransportError(f"read failed: {exc}") from exc
        if not chunk:
            raise Timeout(f"nothing arrived within {timeout:.2f}s")
        return chunk

    def read(self, count: int, timeout: float = DEFAULT_TIMEOUT) -> bytes:
        port = self._require()
        deadline = _deadline(timeout)
        chunks: list[bytes] = []
        got = 0
        while got < count:
            remaining = _remaining(deadline)
            if remaining <= 0:
                raise Timeout(f"read timed out after {timeout:.2f}s ({got}/{count} bytes)")
            port.timeout = remaining
            try:
                chunk = port.read(count - got)
            except Exception as exc:
                raise TransportError(f"read failed: {exc}") from exc
            if not chunk:
                continue
            chunks.append(chunk)
            got += len(chunk)
        return b"".join(chunks)

    def describe(self) -> str:
        return f"{self.port} @ {self.baud}"


def _deadline(timeout: float) -> float:
    return time.monotonic() + timeout


def _remaining(deadline: float) -> float:
    return deadline - time.monotonic()
