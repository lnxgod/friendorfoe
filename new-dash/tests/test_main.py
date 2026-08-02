from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import importlib
import io
from pathlib import Path
import signal
import threading
import time
import unittest
from unittest.mock import patch

from new_dash import __main__ as launcher


class _Lifecycle:
    def __init__(self) -> None:
        self.events: list[str] = []
        self.store_options: dict[str, object] = {}
        self.transport_options: dict[str, object] = {}
        self.application: _Application | None = None
        self.transport: _Transport | None = None

    def store_factory(self, path: Path, **options: object) -> "_Store":
        self.events.append("store.create")
        self.store_options = {"path": path, **options}
        return _Store(self)

    def application_factory(self, store: "_Store") -> "_Application":
        self.events.append("application.create")
        self.application = _Application(self, store)
        return self.application

    def transport_factory(self, **options: object) -> "_Transport":
        self.events.append("transport.create")
        self.transport_options = options
        self.transport = _Transport(self)
        return self.transport

    def server_factory(
        self,
        application: "_Application",
        *,
        requested_port: int | None,
    ) -> "_Server":
        self.events.append(f"server.create:{requested_port}")
        return _Server(self)


class _Store:
    def __init__(self, lifecycle: _Lifecycle) -> None:
        self.lifecycle = lifecycle
        self.closed = False
        self.close_timeout: float | None = None

    def close(self, timeout: float = 3.0) -> None:
        self.lifecycle.events.append("store.close")
        self.close_timeout = timeout
        self.closed = True


class _Application:
    def __init__(self, lifecycle: _Lifecycle, store: _Store) -> None:
        self.lifecycle = lifecycle
        self.store = store
        self.transport: _Transport | None = None
        self.close_timeout: float | None = None

    def attach_transport(self, transport: "_Transport") -> None:
        self.lifecycle.events.append("application.attach_transport")
        self.transport = transport

    def handle_frame(self, frame: object, received_at: float) -> None:
        pass

    def handle_connection(self, update: object) -> None:
        pass

    def close(self, timeout: float = 3.0) -> None:
        self.lifecycle.events.append("application.close")
        self.close_timeout = timeout
        self.store.close(timeout=timeout)


class _Transport:
    def __init__(self, lifecycle: _Lifecycle) -> None:
        self.lifecycle = lifecycle
        self.alive = False
        self.stop_timeout: float | None = None

    def start(self) -> None:
        self.lifecycle.events.append("transport.start")
        self.alive = True

    def stop(self, timeout: float = 3.0) -> None:
        self.lifecycle.events.append("transport.stop")
        self.stop_timeout = timeout
        self.alive = False


class _Thread:
    def __init__(self, lifecycle: _Lifecycle) -> None:
        self.lifecycle = lifecycle
        self.alive = True
        self.join_timeout: float | None = None

    def join(self, timeout: float | None = None) -> None:
        self.lifecycle.events.append("server.join")
        self.join_timeout = timeout
        self.alive = False

    def is_alive(self) -> bool:
        return self.alive


class _Server:
    url = "http://127.0.0.1:8765"

    def __init__(self, lifecycle: _Lifecycle) -> None:
        self.lifecycle = lifecycle
        self.thread = _Thread(lifecycle)

    def serve_in_thread(self) -> _Thread:
        self.lifecycle.events.append("server.start")
        return self.thread

    def shutdown(self) -> None:
        self.lifecycle.events.append("server.shutdown")

    def server_close(self) -> None:
        self.lifecycle.events.append("server.close")


class _BlockingShutdownServer(_Server):
    def __init__(self, lifecycle: _Lifecycle) -> None:
        super().__init__(lifecycle)
        self.shutdown_started = threading.Event()
        self.release_shutdown = threading.Event()
        self.shutdown_finished = threading.Event()

    def shutdown(self) -> None:
        self.lifecycle.events.append("server.shutdown")
        self.shutdown_started.set()
        self.release_shutdown.wait(0.3)
        self.shutdown_finished.set()


class LauncherArgumentTest(unittest.TestCase):
    def test_python_below_311_receives_clear_startup_error(self) -> None:
        stderr = io.StringIO()
        with patch.object(launcher.sys, "version_info", (3, 10, 14)):
            with redirect_stderr(stderr):
                result = launcher.main([])

        self.assertEqual(result, 1)
        self.assertIn("Python 3.11 or newer", stderr.getvalue())

    def test_python_version_error_precedes_runtime_component_imports(self) -> None:
        imported_runtime_modules: list[str] = []
        original_import = __import__

        def tracking_import(
            name: str,
            globals: dict[str, object] | None = None,
            locals: dict[str, object] | None = None,
            fromlist: tuple[str, ...] = (),
            level: int = 0,
        ) -> object:
            if level == 1 and name in {
                "application",
                "serial_transport",
                "storage",
                "web",
            }:
                imported_runtime_modules.append(name)
            return original_import(name, globals, locals, fromlist, level)

        try:
            with patch.object(launcher.sys, "version_info", (3, 10, 14)):
                with patch("builtins.__import__", side_effect=tracking_import):
                    reloaded = importlib.reload(launcher)
                    with redirect_stderr(io.StringIO()):
                        self.assertEqual(reloaded.main([]), 1)
        finally:
            importlib.reload(launcher)

        self.assertEqual(imported_runtime_modules, [])

    def test_defaults_match_local_single_badge_contract(self) -> None:
        args = launcher.parse_args([])

        self.assertIsNone(args.port)
        self.assertIsNone(args.http_port)
        self.assertFalse(args.no_browser)
        self.assertIsNone(args.data_dir)
        self.assertEqual(args.retention_days, 30)
        self.assertEqual(args.max_observations, 50_000)

    def test_default_database_path_uses_the_macos_application_support_folder(self) -> None:
        with patch.object(
            launcher.Path, "home", return_value=Path("/Users/new-dash-test")
        ):
            database = launcher.database_path(None)

        self.assertEqual(
            database,
            Path(
                "/Users/new-dash-test/Library/Application Support/New Dash/"
                "new-dash.sqlite3"
            ),
        )

    def test_explicit_data_directory_contains_the_database_file(self) -> None:
        self.assertEqual(
            launcher.database_path(Path("/private/tmp/new-dash-data")),
            Path("/private/tmp/new-dash-data/new-dash.sqlite3"),
        )

    def test_invalid_positive_counts_and_http_ports_are_rejected(self) -> None:
        invalid_arguments = (
            ("--retention-days", "0"),
            ("--max-observations", "0"),
            ("--http-port", "0"),
            ("--http-port", "65536"),
        )
        for arguments in invalid_arguments:
            with self.subTest(arguments=arguments):
                with redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        launcher.parse_args(list(arguments))

    def test_abbreviated_option_names_are_rejected(self) -> None:
        abbreviated_arguments = (
            ("--po", "/dev/cu.usbmodem101"),
            ("--http", "8123"),
            ("--no",),
            ("--data", "/private/tmp/new-dash"),
            ("--retention", "7"),
            ("--max", "99"),
        )
        for arguments in abbreviated_arguments:
            with self.subTest(arguments=arguments):
                with redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        launcher.parse_args(list(arguments))

    def test_remote_bind_flags_are_not_accepted(self) -> None:
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                launcher.parse_args(["--host", "0.0.0.0"])


class LauncherLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.lifecycle = _Lifecycle()
        self.patches = (
            patch.object(launcher, "ObservationStore", self.lifecycle.store_factory),
            patch.object(
                launcher, "NewDashApplication", self.lifecycle.application_factory
            ),
            patch.object(
                launcher, "BadgeSerialTransport", self.lifecycle.transport_factory
            ),
            patch.object(launcher, "create_http_server", self.lifecycle.server_factory),
        )
        for active_patch in self.patches:
            active_patch.start()

    def tearDown(self) -> None:
        for active_patch in reversed(self.patches):
            active_patch.stop()

    def test_normal_composition_opens_after_start_and_closes_in_reverse_order(self) -> None:
        output = io.StringIO()

        def open_browser(url: str) -> bool:
            self.lifecycle.events.append(f"browser.open:{url}")
            return True

        args = launcher.parse_args(
            [
                "--port",
                "/dev/cu.usbmodem101",
                "--http-port",
                "8123",
                "--data-dir",
                "/private/tmp/new-dash",
                "--retention-days",
                "9",
                "--max-observations",
                "1234",
            ]
        )
        with patch.object(launcher, "_wait_for_shutdown", return_value=None):
            with patch.object(launcher.webbrowser, "open", side_effect=open_browser):
                with redirect_stdout(output):
                    launcher.run(args)

        self.assertEqual(
            self.lifecycle.store_options,
            {
                "path": Path("/private/tmp/new-dash/new-dash.sqlite3"),
                "retention_days": 9,
                "max_observations": 1234,
            },
        )
        self.assertEqual(
            set(self.lifecycle.transport_options),
            {"explicit_port", "on_frame", "on_connection"},
        )
        self.assertEqual(
            self.lifecycle.transport_options["explicit_port"],
            "/dev/cu.usbmodem101",
        )
        self.assertIs(
            self.lifecycle.transport_options["on_frame"].__self__,
            self.lifecycle.application,
        )
        self.assertIs(
            self.lifecycle.transport_options["on_connection"].__self__,
            self.lifecycle.application,
        )
        events = self.lifecycle.events
        self.assertLess(events.index("server.start"), events.index("browser.open:http://127.0.0.1:8765"))
        self.assertEqual(
            events[-6:],
            [
                "server.shutdown",
                "server.close",
                "server.join",
                "transport.stop",
                "application.close",
                "store.close",
            ],
        )
        self.assertIsNotNone(self.lifecycle.application)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertGreater(self.lifecycle.application.close_timeout, 0.0)
        self.assertLessEqual(self.lifecycle.application.close_timeout, 3.0)
        self.assertGreater(self.lifecycle.transport.stop_timeout, 0.0)
        self.assertLessEqual(self.lifecycle.transport.stop_timeout, 3.0)
        self.assertIn("http://127.0.0.1:8765", output.getvalue())
        self.assertIn("/private/tmp/new-dash/new-dash.sqlite3", output.getvalue())

    def test_cleanup_components_share_one_deadline(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        server = _Server(self.lifecycle)
        original_shutdown = server.shutdown

        def slow_shutdown() -> None:
            time.sleep(0.03)
            original_shutdown()

        server.shutdown = slow_shutdown  # type: ignore[method-assign]
        with patch.object(launcher, "create_http_server", return_value=server):
            with patch.object(launcher, "_SHUTDOWN_TIMEOUT_SECONDS", 0.05):
                with patch.object(launcher, "_wait_for_shutdown", return_value=None):
                    with redirect_stdout(io.StringIO()):
                        launcher.run(args)

        self.assertIsNotNone(self.lifecycle.application)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertIsNotNone(server.thread.join_timeout)
        self.assertIsNotNone(self.lifecycle.transport.stop_timeout)
        self.assertIsNotNone(self.lifecycle.application.close_timeout)
        self.assertLess(server.thread.join_timeout, 0.03)
        self.assertLessEqual(
            self.lifecycle.transport.stop_timeout,
            server.thread.join_timeout,
        )
        self.assertLessEqual(
            self.lifecycle.application.close_timeout,
            self.lifecycle.transport.stop_timeout,
        )

    def test_blocked_http_shutdown_cannot_skip_later_cleanup(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        server = _BlockingShutdownServer(self.lifecycle)

        try:
            started = time.monotonic()
            with patch.object(launcher, "create_http_server", return_value=server):
                with patch.object(launcher, "_SHUTDOWN_TIMEOUT_SECONDS", 0.05):
                    with patch.object(launcher, "_wait_for_shutdown", return_value=None):
                        with redirect_stdout(io.StringIO()):
                            launcher.run(args)
            elapsed = time.monotonic() - started

            self.assertTrue(server.shutdown_started.is_set())
            self.assertFalse(server.shutdown_finished.is_set())
            self.assertLess(elapsed, 0.2)
            self.assertIn("server.close", self.lifecycle.events)
            self.assertIn("transport.stop", self.lifecycle.events)
            self.assertIn("application.close", self.lifecycle.events)
            self.assertIn("store.close", self.lifecycle.events)
        finally:
            server.release_shutdown.set()

    def test_no_browser_suppresses_browser_open(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        with patch.object(launcher, "_wait_for_shutdown", return_value=None):
            with patch.object(
                launcher.webbrowser,
                "open",
                side_effect=AssertionError("browser must remain closed"),
            ):
                with redirect_stdout(io.StringIO()):
                    launcher.run(args)

    def test_keyboard_interrupt_still_stops_every_worker(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        with patch.object(
            launcher, "_wait_for_shutdown", side_effect=KeyboardInterrupt
        ):
            with redirect_stdout(io.StringIO()):
                launcher.run(args)

        self.assertEqual(self.lifecycle.events.count("transport.start"), 1)
        self.assertIn("transport.stop", self.lifecycle.events)
        self.assertIn("server.join", self.lifecycle.events)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertFalse(self.lifecycle.transport.alive)

    def test_keyboard_interrupt_during_browser_open_ends_cleanly(self) -> None:
        args = launcher.parse_args([])
        with patch.object(
            launcher.webbrowser, "open", side_effect=KeyboardInterrupt
        ):
            with patch.object(
                launcher,
                "_wait_for_shutdown",
                side_effect=AssertionError("wait must not run after browser interruption"),
            ):
                with redirect_stdout(io.StringIO()):
                    launcher.run(args)

        self.assertIn("transport.stop", self.lifecycle.events)
        self.assertIn("server.join", self.lifecycle.events)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertFalse(self.lifecycle.transport.alive)

    def test_sigint_sets_shutdown_event_and_stops_every_worker(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        handlers: dict[int, object] = {}

        def install_handler(signum: int, handler: object) -> object:
            previous = handlers.get(signum, signal.default_int_handler)
            handlers[signum] = handler
            return previous

        def send_sigint(stop_event: object) -> None:
            handler = handlers[signal.SIGINT]
            self.assertTrue(callable(handler))
            handler(signal.SIGINT, None)
            self.assertTrue(stop_event.is_set())

        with patch.object(launcher.signal, "signal", side_effect=install_handler):
            with patch.object(launcher, "_wait_for_shutdown", side_effect=send_sigint):
                with redirect_stdout(io.StringIO()):
                    launcher.run(args)

        self.assertIn("transport.stop", self.lifecycle.events)
        self.assertIn("server.join", self.lifecycle.events)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertFalse(self.lifecycle.transport.alive)

    def test_sigterm_uses_the_same_bounded_shutdown_path(self) -> None:
        args = launcher.parse_args(["--no-browser"])
        handlers: dict[int, object] = {}

        def install_handler(signum: int, handler: object) -> object:
            previous = handlers.get(signum, signal.SIG_DFL)
            handlers[signum] = handler
            return previous

        def send_sigterm(stop_event: object) -> None:
            handler = handlers[signal.SIGTERM]
            self.assertTrue(callable(handler))
            handler(signal.SIGTERM, None)
            self.assertTrue(stop_event.is_set())

        with patch.object(launcher.signal, "signal", side_effect=install_handler):
            with patch.object(launcher, "_wait_for_shutdown", side_effect=send_sigterm):
                with redirect_stdout(io.StringIO()):
                    launcher.run(args)

        self.assertIn("transport.stop", self.lifecycle.events)
        self.assertIn("server.join", self.lifecycle.events)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertFalse(self.lifecycle.transport.alive)

    def test_startup_failure_stops_the_serial_worker_and_closes_owned_resources(self) -> None:
        args = launcher.parse_args(["--no-browser"])

        def fail_to_serve() -> object:
            self.lifecycle.events.append("server.start.failed")
            raise RuntimeError("HTTP worker did not start")

        server = _Server(self.lifecycle)
        server.serve_in_thread = fail_to_serve  # type: ignore[method-assign]
        with patch.object(launcher, "create_http_server", return_value=server):
            with redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "HTTP worker did not start"):
                    launcher.run(args)

        self.assertIn("transport.start", self.lifecycle.events)
        self.assertIn("server.close", self.lifecycle.events)
        self.assertIn("transport.stop", self.lifecycle.events)
        self.assertIn("application.close", self.lifecycle.events)
        self.assertIsNotNone(self.lifecycle.transport)
        self.assertFalse(self.lifecycle.transport.alive)


if __name__ == "__main__":
    unittest.main()
