"""A real ECU, in a Python process, reachable over the real link protocol.

The point of this module is that the tuner, the client, and the whole flash workflow can be
driven end to end with no hardware: no serial port, no WiFi, no engine, and no stubbed-out
commands. It speaks the same frames over the same :class:`~raccoon.transport.Transport`
interface, so a bug found here is a bug in the tuner, and a fix here is a fix that holds on
the bench.

It wraps a compiled `rc_sim` subprocess rather than reimplementing the ECU in Python. A
Python reimplementation would be a second ECU, free to agree with whatever the client
expects - and every one of the defects this project has already found (the scheduler that
never fired an event, the fuel model in barometric units, a validator that did nothing)
lived in the gap between what the client expected and what the firmware actually did. Two
independent implementations cannot find that class of bug. One implementation and one
client can.
"""

from __future__ import annotations

import pathlib
import socket
import subprocess
import time

from raccoon.transport import Transport, TransportError

ROOT = pathlib.Path(__file__).resolve().parents[2]


class BridgeError(Exception):
    """The bridge could not start or has died."""


class _Simulator:
    """A running `rc_sim` with the link server attached."""

    def __init__(self, binary: pathlib.Path, extra: list[str] | None = None,
                 log_path: pathlib.Path | str | None = None) -> None:
        self.binary = binary
        self.extra = extra or []
        self.log_path = pathlib.Path(log_path) if log_path else None
        self._log = None
        self._log_offset = 0
        self.process: subprocess.Popen | None = None
        self.port: int | None = None
        self.control_port: int | None = None

    def start(self, timeout: float = 20.0) -> int:
        if not self.binary.exists():
            raise BridgeError(
                f"{self.binary} does not exist. Build it first:\n"
                f"  cmake -S . -B build && cmake --build build -j"
            )
        # The simulator's output goes to a file when one is given, and to a pipe otherwise.
        # A pipe is only safe while something is draining it: the rig writes to stderr for
        # the same reasons a test rig always does, and a full pipe blocks the ECU's service
        # loop. The failure looks like the ECU hanging, which is the least likely possible
        # explanation and the hardest to see.
        if self.log_path is not None:
            self._log = self.log_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen(
            [str(self.binary), "--serve-link", *self.extra],
            stdout=self._log if self._log is not None else subprocess.PIPE,
            stderr=subprocess.STDOUT if self._log is not None else None,
            text=True,
            bufsize=1,
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                output = ""
                if self.process.stdout is not None:
                    output = self.process.stdout.read()
                elif self.log_path is not None:
                    output = self.log_path.read_text(errors="replace")
                raise BridgeError(
                    f"the simulator exited immediately with code {self.process.returncode}:\n"
                    f"{output.strip()}"
                )
            line = self._read_line(deadline)
            if line is None:
                continue
            if line.startswith("LINK_PORT "):
                self.port = int(line.split()[1])
            elif line.startswith("CONTROL_PORT "):
                self.control_port = int(line.split()[1])
            if self.port is not None and self.control_port is not None:
                return self.port
        self.stop()
        raise BridgeError(f"the simulator did not announce a link port within {timeout:.0f}s")

    def _read_line(self, deadline: float) -> str | None:
        """One line of the rig's output, from whichever sink it is using.

        The port announcement is how a parent finds a server on an arbitrary port, so it has
        to be read from the same place the rig writes - a pipe when there is one, the log
        file otherwise. Reading the wrong one is how this ends up waiting for a stream nobody
        will ever write to.
        """
        if self.process is None:
            return None
        if self.process.stdout is not None:
            line = self.process.stdout.readline()
            return line.strip() if line else None
        # File sink: read forward from where the last read stopped.
        while time.monotonic() < deadline:
            if self.log_path is None or not self.log_path.exists():
                time.sleep(0.01)
                continue
            with self.log_path.open("r", encoding="utf-8", errors="replace") as handle:
                handle.seek(self._log_offset)
                line = handle.readline()
                if line:
                    self._log_offset = handle.tell()
                    return line.strip()
            if self.process.poll() is not None:
                return None
            time.sleep(0.01)
        return None

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        if self.process.stdout is not None:
            self.process.stdout.close()
        if self._log is not None:
            self._log.close()
            self._log = None
        self.process = None


class _Transport(Transport):
    """Talks to the simulator's TCP link, but on a thread the GUI can abandon.

    A read that is already in flight when the operator closes the link must not keep the GUI
    waiting, so the socket is shut down underneath the reader rather than joined. That is the
    one way to unblock a thread parked in ``recv``, and it is why the reader catches
    ``OSError`` and reports "closed" rather than a failure.
    """

    def __init__(self, port: int, timeout: float = 1.0) -> None:
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._closed = False

    def open(self) -> None:
        if self._sock is not None:
            return
        try:
            self._sock = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
        except OSError as exc:
            raise TransportError(f"cannot reach the simulator on port {self.port}: {exc}") from exc
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._closed = False

    def close(self) -> None:
        self._closed = True
        sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def write(self, data: bytes, timeout: float | None = None) -> None:
        if self._sock is None:
            raise TransportError("the link is not open")
        # `timeout or default` would turn a legitimate 0 into the default: a poll
        # asking for "nothing, right now" would block for a second, and a timer
        # firing every 50 ms would never let the worker's own queued work run.
        self._sock.settimeout(self.timeout if timeout is None else timeout)
        try:
            self._sock.sendall(data)
        except socket.timeout as exc:
            wait = self.timeout if timeout is None else timeout
            raise TransportError(f"write timed out after {wait:.2f}s") from exc
        except OSError as exc:
            raise TransportError(f"write failed: {exc}") from exc

    def read_available(self, limit: int, timeout: float | None = None) -> bytes:
        """Up to ``limit`` bytes, blocking only until at least one arrives.

        See the note in :mod:`raccoon.transport`: asking a stream for an exact byte count
        deadlocks against a frame-based protocol whenever the frame is shorter than the ask.
        """
        if self._sock is None:
            raise TransportError("the link is not open")
        # `timeout or default` would turn a legitimate 0 into the default: a poll
        # asking for "nothing, right now" would block for a second, and a timer
        # firing every 50 ms would never let the worker's own queued work run.
        self._sock.settimeout(self.timeout if timeout is None else timeout)
        try:
            chunk = self._sock.recv(limit)
        except socket.timeout as exc:
            wait = self.timeout if timeout is None else timeout
            raise TransportError(f"nothing arrived within {wait:.2f}s") from exc
        except OSError as exc:
            if self._closed:
                raise TransportError("the link was closed") from exc
            raise TransportError(f"read failed: {exc}") from exc
        if not chunk:
            raise TransportError("the simulator closed the link")
        return chunk

    def read(self, count: int, timeout: float | None = None) -> bytes:
        if self._sock is None:
            raise TransportError("the link is not open")
        # `timeout or default` would turn a legitimate 0 into the default: a poll
        # asking for "nothing, right now" would block for a second, and a timer
        # firing every 50 ms would never let the worker's own queued work run.
        self._sock.settimeout(self.timeout if timeout is None else timeout)
        chunks: list[bytes] = []
        got = 0
        try:
            while got < count:
                chunk = self._sock.recv(count - got)
                if not chunk:
                    raise TransportError("the simulator closed the link")
                chunks.append(chunk)
                got += len(chunk)
        except socket.timeout as exc:
            raise TransportError(f"read timed out ({got}/{count} bytes)") from exc
        except OSError as exc:
            if self._closed:
                raise TransportError("the link was closed") from exc
            raise TransportError(f"read failed: {exc}") from exc
        return b"".join(chunks)

    def describe(self) -> str:
        return f"simulator:{self.port}"


class SimulatorBridge:
    """Owns the simulator process and hands out a transport for it.

    Use as a context manager, or call :meth:`start` and :meth:`stop`. The process is a
    child, so it is terminated on stop - leaving a simulator running after a GUI exits would
    make the next connection fail for a reason that has nothing to do with the code.
    """

    def __init__(self, binary: pathlib.Path | None = None, extra: list[str] | None = None,
                 log_path: pathlib.Path | str | None = None) -> None:
        self.binary = binary or (ROOT / "build" / "rc_sim")
        self.extra = extra or []
        self.sim = _Simulator(self.binary, self.extra, log_path)
        self.transport: _Transport | None = None
        self.control_port: int | None = None
        self._control: socket.socket | None = None
        self._reap_at_exit()

    def engine(self, command: str) -> str:
        """Start or stop the simulated engine, the way an operator would with a key.

        Not part of the tuner protocol, and deliberately so: a client that could stop a
        running engine over the same link that is refused a tune change because the engine is
        running would make the NOT_ARMED gate meaningless. The simulator offers it on a
        separate port precisely to keep that separation honest.
        """
        if self.control_port is None:
            raise BridgeError("the simulator has no control port")
        # One connection, kept open. The rig's console is a line channel, and reconnecting
        # per command would race the simulator's accept: the command can be written before
        # it has accepted, and the reply then belongs to a connection nobody is reading.
        if self._control is None:
            self._control = socket.create_connection(
                ("127.0.0.1", self.control_port), timeout=3.0
            )
            self._control.settimeout(3.0)
        self._control.sendall((command + "\n").encode())
        reply = b""
        while b"\n" not in reply:
            chunk = self._control.recv(64)
            if not chunk:
                raise BridgeError("the simulator's control channel closed")
            reply += chunk
        return reply.decode(errors="replace").strip()

    def stop_engine(self) -> str:
        return self.engine("stop")

    def start_engine(self) -> str:
        return self.engine("start")

    def launch(self) -> int:
        """Start the simulator and return its link port, without connecting to it.

        Separate from :meth:`start` because the simulator serves ONE client at a time: a
        transport opened here and then abandoned still holds the ECU's only endpoint, the
        ECU streams live data into a socket nobody reads, and the first real client sees a
        link that appears to accept connections and answer nothing. The symptom looks like a
        network fault and is actually a leftover handle.
        """
        self.sim.start()
        self.control_port = self.sim.control_port
        return self.sim.port

    def connect(self, timeout: float = 1.0) -> Transport:
        """Connect to an already-launched simulator."""
        if self.transport is None:
            if self.sim.port is None:
                self.launch()
            self.transport = _Transport(self.sim.port, timeout)
            self.transport.open()
        return self.transport

    def start(self) -> Transport:
        self.launch()
        return self.connect()

    def _reap_at_exit(self) -> None:
        """Last-resort child cleanup, registered on construction.

        The window already shuts the bridge down on close and on `aboutToQuit`, and both of
        those work. Neither covers the case that actually bites: a process killed by a
        signal, or one that dies inside a callback, where no Qt teardown runs at all and the
        child simulator is left running with its engine turning. The next person to start a
        tuner then finds a port in use and an ECU that is mysteriously already warm.

        `atexit` covers every path the interpreter gets to. A SIGKILL gets no cleanup by
        anything, in any language - the parent dying should be rare enough to be worth
        seeing.
        """
        import atexit

        atexit.register(self.stop)

    def stop(self) -> None:
        if self._control is not None:
            self._control.close()
            self._control = None
        if self.transport is not None:
            self.transport.close()
            self.transport = None
        self.sim.stop()

    def __enter__(self) -> Transport:
        return self.start()

    def __exit__(self, *_exc) -> None:
        self.stop()


def python_transport(host: str, port: int, timeout: float = 1.0) -> Transport:
    """A transport to an already-running simulator or ECU over TCP.

    Thin on purpose: everything above :class:`Transport` is identical whether the far end is
    a laptop, a bench, or the simulator in this process's sibling.
    """
    from raccoon.transport import TcpTransport

    return TcpTransport(host, port, timeout)
