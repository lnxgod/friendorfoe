"""Source-first launcher for the local New Dash USB dashboard."""

from __future__ import annotations

import argparse
from pathlib import Path
import signal
import sys
import threading
import time
from types import FrameType
from typing import Any, Sequence
import webbrowser

_DATABASE_NAME = "new-dash.sqlite3"
_SHUTDOWN_TIMEOUT_SECONDS = 3.0

# Runtime components stay lazy so an unsupported interpreter can print the
# version error before importing the application and transport implementation.
NewDashApplication: Any = None
BadgeSerialTransport: Any = None
ObservationStore: Any = None
create_http_server: Any = None


def _positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be at least 1")
    return parsed


def _http_port(value: str) -> int:
    parsed = int(value)
    if not 1 <= parsed <= 65_535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return parsed


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse the small, loopback-only New Dash command-line surface."""

    parser = argparse.ArgumentParser(
        prog="python -m new_dash",
        description="Run New Dash for one Friend or Foe badge over USB.",
        allow_abbrev=False,
    )
    parser.add_argument("--port", help="explicit badge serial device path")
    parser.add_argument(
        "--http-port",
        type=_http_port,
        help="loopback HTTP port (default: 8765, with an automatic fallback)",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="do not open the dashboard in the default browser",
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        help="directory for the local New Dash SQLite database",
    )
    parser.add_argument(
        "--retention-days",
        type=_positive_integer,
        default=30,
        help="days of local history to retain (default: 30)",
    )
    parser.add_argument(
        "--max-observations",
        type=_positive_integer,
        default=50_000,
        help="maximum local history rows (default: 50000)",
    )
    return parser.parse_args(arguments)


def database_path(data_directory: Path | None) -> Path:
    """Resolve the one SQLite file without inspecting process environment."""

    directory = (
        Path.home() / "Library" / "Application Support" / "New Dash"
        if data_directory is None
        else data_directory.expanduser()
    )
    return directory / _DATABASE_NAME


def _wait_for_shutdown(stop_event: threading.Event) -> None:
    stop_event.wait()


def _bounded_server_shutdown(server: Any, deadline: float) -> None:
    """Request BaseServer shutdown without letting its internal wait escape."""

    errors: list[BaseException] = []

    def request_shutdown() -> None:
        try:
            server.shutdown()
        except BaseException as error:
            errors.append(error)

    shutdown_thread = threading.Thread(
        target=request_shutdown,
        name="new-dash-http-shutdown",
        daemon=True,
    )
    shutdown_thread.start()
    shutdown_thread.join(max(0.0, deadline - time.monotonic()))
    if not shutdown_thread.is_alive() and errors:
        raise errors[0]


def _load_runtime_components() -> None:
    global NewDashApplication
    global BadgeSerialTransport
    global ObservationStore
    global create_http_server

    if NewDashApplication is None:
        from .application import NewDashApplication as application_class

        NewDashApplication = application_class
    if BadgeSerialTransport is None:
        from .serial_transport import BadgeSerialTransport as transport_class

        BadgeSerialTransport = transport_class
    if ObservationStore is None:
        from .storage import ObservationStore as store_class

        ObservationStore = store_class
    if create_http_server is None:
        from .web import create_http_server as server_factory

        create_http_server = server_factory


def run(arguments: argparse.Namespace) -> None:
    """Compose, run, and close one New Dash instance."""

    _load_runtime_components()
    stop_event = threading.Event()

    def request_shutdown(_signum: int, _frame: FrameType | None) -> None:
        stop_event.set()

    previous_handlers: list[tuple[int, Any]] = []
    store = None
    application = None
    transport = None
    server = None
    server_thread: threading.Thread | None = None
    path = database_path(arguments.data_dir)

    try:
        for shutdown_signal in (signal.SIGINT, signal.SIGTERM):
            previous_handlers.append(
                (shutdown_signal, signal.signal(shutdown_signal, request_shutdown))
            )
        store = ObservationStore(
            path,
            retention_days=arguments.retention_days,
            max_observations=arguments.max_observations,
        )
        application = NewDashApplication(store)
        transport = BadgeSerialTransport(
            explicit_port=arguments.port,
            on_frame=application.handle_frame,
            on_connection=application.handle_connection,
        )
        application.attach_transport(transport)
        server = create_http_server(
            application,
            requested_port=arguments.http_port,
        )
        transport.start()
        server_thread = server.serve_in_thread()

        print(f"New Dash data: {path}")
        print(f"New Dash: {server.url}")
        if not arguments.no_browser:
            webbrowser.open(server.url)
        _wait_for_shutdown(stop_event)
    except KeyboardInterrupt:
        stop_event.set()
    finally:
        shutdown_deadline = time.monotonic() + _SHUTDOWN_TIMEOUT_SECONDS

        def remaining_shutdown_time() -> float:
            return max(0.0, shutdown_deadline - time.monotonic())

        try:
            if server is not None:
                if server_thread is not None:
                    try:
                        _bounded_server_shutdown(server, shutdown_deadline)
                    finally:
                        try:
                            server.server_close()
                        finally:
                            server_thread.join(remaining_shutdown_time())
                else:
                    server.server_close()
        finally:
            try:
                if transport is not None:
                    transport.stop(timeout=remaining_shutdown_time())
            finally:
                try:
                    if application is not None:
                        application.close(timeout=remaining_shutdown_time())
                    elif store is not None:
                        store.close(timeout=remaining_shutdown_time())
                finally:
                    for shutdown_signal, previous_handler in reversed(
                        previous_handlers
                    ):
                        signal.signal(shutdown_signal, previous_handler)


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the CLI and return a process exit status."""

    if sys.version_info < (3, 11):
        print("New Dash requires Python 3.11 or newer.", file=sys.stderr)
        return 1
    parsed = parse_args(arguments)
    try:
        run(parsed)
    except Exception as error:
        print(f"New Dash could not start: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
