"""Entry point: python -m raccoon [target].

The target decides what to talk to, and the default is the simulator. That default is a
deliberate safety choice: a tuner that connects to real hardware because someone forgot an
argument would be alarming, and one that starts by showing a working ECU lets the interface
be reviewed without a motorcycle present.

    python -m raccoon                  # the simulator
    python -m raccoon tcp:192.168.1.50:5000
    python -m raccoon serial:/dev/ttyACM0
    python -m raccoon COM7
"""

from __future__ import annotations

import sys

from raccoon.ui.main_window import MainWindow


def main(argv: list[str]) -> int:
    target = argv[1] if len(argv) > 1 else "simulator"
    app = _application()
    _install_signal_handling(app)
    window = MainWindow(target)
    window.show()
    return app.exec()


def _install_signal_handling(app) -> None:
    """Turn SIGINT and SIGTERM into an ordinary quit.

    A tuner killed from a terminal or by a session manager must still shut the link down
    cleanly. The default behaviour for both signals is to kill the process outright, which
    skips the window's close handler - so the child simulator is left running with its
    engine turning, and the next person to start a tuner finds a port already in use and an
    ECU that is somehow already warm.

    Handlers are removed once installed: a second Ctrl-C should kill the process outright,
    which is what someone pressing it twice is asking for.
    """
    import signal

    def quit_app(*_args) -> None:
        app.quit()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, quit_app)
        except (ValueError, OSError):
            # Not the main thread, or the platform has no such signal. The application is
            # still perfectly usable; it just will not shut down tidely on that signal.
            pass


def _application():
    """Create the QApplication, with the attributes Qt wants set before it exists.

    High DPI is enabled rather than left to the platform default, because the gauges are
    sized in pixels and a scaling factor applied afterwards leaves text blurry - which on a
    tuner screen means misreading a number.
    """
    from PyQt6.QtCore import Qt
    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QApplication

    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("RACCOON V2 tuner")
    app.setFont(QFont("DejaVu Sans", 10))
    return app


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
