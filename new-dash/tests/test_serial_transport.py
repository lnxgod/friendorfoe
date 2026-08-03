import json
import threading
import time
import unittest

from new_dash.controls import build_display_nav, build_theme
from new_dash.serial_transport import (
    BadgeSerialTransport,
    ConnectionUpdate,
    ControlTimeout,
    DiscoveryError,
    PortIdentity,
    TransportError,
    TransportUnavailable,
    UnsupportedCapability,
    choose_candidate,
    discover_badge_ports,
)

if __package__:
    from .fakes import FakeSerial, ManualClock, StepClock
else:
    from fakes import FakeSerial, ManualClock, StepClock


ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001


def lite_status(version: str = "0.2.0-backend", **overrides: object) -> bytes:
    payload: dict[str, object] = {
        "product_family": "badge_lite",
        "target": "uplink-s3-backend",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "version": version,
        "mode": "headless",
        "capabilities": [
            "display_none", "usb_live", "usb_live_ack", "usb_buffered",
            "usb_config", "http_uplink", "config_ap",
        ],
        "scanner": [],
        "scanner_summaries": [],
    }
    payload.update(overrides)
    return b"FOF_STATUS:" + json.dumps(payload, separators=(",", ":")).encode() + b"\n"


def native_status(version: str = "v-live") -> bytes:
    payload = {
        "version": version,
        "target": "uplink-s3-fof_badge",
        "project": "fof_badge_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "mode": "usb_only",
        "entities": [],
        "scanners": [],
    }
    return b"FOF_STATUS:" + json.dumps(payload, separators=(",", ":")).encode() + b"\n"


def espressif(
    suffix: str,
    *,
    prefix: str = "cu",
    serial_number: str | None = None,
    location: str | None = None,
) -> PortIdentity:
    return PortIdentity(
        f"/dev/{prefix}.usbmodem{suffix}",
        ESPRESSIF_VID,
        USB_SERIAL_JTAG_PID,
        "Espressif",
        "USB JTAG/serial debug unit",
        serial_number,
        location,
    )


class DiscoveryTest(unittest.TestCase):
    def test_exactly_one_espressif_candidate_is_selected(self) -> None:
        ports = [
            PortIdentity(
                "/dev/cu.Bluetooth", 0x05AC, 1, "Apple", "Bluetooth", None, None
            ),
            espressif("101", serial_number="ABC", location="1-1"),
        ]

        self.assertEqual(choose_candidate(ports).device, "/dev/cu.usbmodem101")

    def test_zero_and_multiple_candidates_do_not_guess(self) -> None:
        with self.assertRaises(DiscoveryError) as no_badge:
            choose_candidate([])
        self.assertEqual(no_badge.exception.code, "no_badge")
        self.assertEqual(no_badge.exception.candidates, ())

        with self.assertRaises(DiscoveryError) as multiple:
            choose_candidate([espressif("101"), espressif("201")])
        self.assertEqual(multiple.exception.code, "multiple_badges")
        self.assertEqual(
            multiple.exception.candidates,
            (("/dev/cu.usbmodem101",), ("/dev/cu.usbmodem201",)),
        )

    def test_automatic_match_requires_both_exact_numeric_ids(self) -> None:
        mismatches = (
            PortIdentity(
                "/dev/cu.right-vid",
                ESPRESSIF_VID,
                0x9999,
                "Espressif",
                "USB",
                None,
                None,
            ),
            PortIdentity(
                "/dev/cu.right-pid",
                0x9999,
                USB_SERIAL_JTAG_PID,
                "Espressif",
                "USB",
                None,
                None,
            ),
            PortIdentity(
                "/dev/cu.text-only",
                None,
                None,
                "Espressif",
                "USB JTAG/serial",
                None,
                None,
            ),
        )
        with self.assertRaises(DiscoveryError) as raised:
            choose_candidate(mismatches)
        self.assertEqual(raised.exception.code, "no_badge")

    def test_explicit_path_is_selected_without_usb_metadata(self) -> None:
        explicit = PortIdentity(
            "/dev/cu.custom-badge", None, None, None, None, None, None
        )

        selected = choose_candidate([explicit], explicit_path=explicit.device)

        self.assertEqual(selected, explicit)

    def test_stable_aliases_count_as_one_physical_candidate_and_prefer_cu(self) -> None:
        aliases = [
            espressif("101", prefix="tty", serial_number="ABC", location="1-1"),
            espressif("101", prefix="cu", serial_number="ABC", location="1-1"),
        ]

        selected = choose_candidate(aliases)

        self.assertEqual(selected.device, "/dev/cu.usbmodem101")

    def test_two_physical_exact_matches_remain_ambiguous(self) -> None:
        ports = [
            espressif("101", serial_number="ABC", location="1-1"),
            espressif("201", serial_number="DEF", location="1-2"),
        ]

        with self.assertRaises(DiscoveryError) as raised:
            choose_candidate(ports)

        self.assertEqual(raised.exception.code, "multiple_badges")
        self.assertEqual(len(raised.exception.candidates), 2)

    def test_missing_stable_fields_deduplicate_only_exact_cu_tty_suffix_pair(self) -> None:
        aliases = [espressif("101", prefix="tty"), espressif("101", prefix="cu")]
        self.assertEqual(choose_candidate(aliases).device, "/dev/cu.usbmodem101")

        with self.assertRaises(DiscoveryError) as raised:
            choose_candidate(
                [espressif("101", prefix="cu"), espressif("201", prefix="tty")]
            )
        self.assertEqual(raised.exception.code, "multiple_badges")

    def test_adapter_copies_pyserial_metadata_into_immutable_identities(self) -> None:
        class ListPort:
            device = "/dev/cu.usbmodem101"
            vid = ESPRESSIF_VID
            pid = USB_SERIAL_JTAG_PID
            manufacturer = "Espressif"
            product = "USB JTAG/serial"
            serial_number = "ABC"
            location = "1-1"

        discovered = discover_badge_ports(lambda: [ListPort()])

        self.assertEqual(
            discovered,
            (
                PortIdentity(
                    "/dev/cu.usbmodem101",
                    ESPRESSIF_VID,
                    USB_SERIAL_JTAG_PID,
                    "Espressif",
                    "USB JTAG/serial",
                    "ABC",
                    "1-1",
                ),
            ),
        )
        with self.assertRaises((AttributeError, TypeError)):
            discovered[0].device = "/dev/cu.changed"  # type: ignore[misc]


class VerifiedSessionTest(unittest.TestCase):
    def test_factory_compatible_configuration_leaves_control_lines_at_defaults(self) -> None:
        fake = FakeSerial([b"I (12) boot\nFOF_PO", b"NG:v1.2.3\n"])
        updates: list[ConnectionUpdate] = []
        live = threading.Event()
        factory_open_states: list[bool] = []
        worker_daemon_states: list[bool] = []

        def serial_factory() -> FakeSerial:
            factory_open_states.append(fake.is_open)
            worker_daemon_states.append(threading.current_thread().daemon)
            return fake

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "live":
                live.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=serial_factory,
            on_connection=on_connection,
        )
        transport.start()
        self.assertTrue(live.wait(1.0), updates)

        self.assertEqual(factory_open_states, [False])
        self.assertEqual(worker_daemon_states, [False])
        expected_before_ping = [
            ("set", "port", "/dev/cu.usbmodem101"),
            ("set", "baudrate", 115200),
            ("set", "timeout", 0.1),
            ("set", "write_timeout", 3.0),
            ("set", "exclusive", True),
            ("open",),
            ("reset_input_buffer",),
        ]
        self.assertEqual(fake.actions[: len(expected_before_ping)], expected_before_ping)
        self.assertEqual(
            fake.actions[len(expected_before_ping)],
            ("write", b"\nFOF_PING\n"),
        )
        control_line_actions = [
            action
            for action in fake.actions
            if action[0] in {"setDTR", "setRTS"}
            or action[:2] in {("set", "dtr"), ("set", "rts")}
        ]
        self.assertEqual(control_line_actions, [])
        self.assertEqual(fake.writes[0], b"\nFOF_PING\n")
        live_update = next(update for update in updates if update.state == "live")
        self.assertEqual(live_update.firmware_version, "v1.2.3")
        with self.assertRaises((AttributeError, TypeError)):
            live_update.state = "changed"  # type: ignore[misc]

        transport.stop()
        self.assertTrue(fake.closed.wait(1.0))

    def test_live_is_not_emitted_until_a_post_send_pong_arrives(self) -> None:
        fake = FakeSerial()
        updates: list[ConnectionUpdate] = []
        live = threading.Event()

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "live":
                live.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=on_connection,
        )
        transport.start()
        self.assertTrue(fake.written.wait(1.0))
        self.assertFalse(live.is_set())

        fake.feed(b"FOF_PONG:verified\n")
        self.assertTrue(live.wait(1.0), updates)
        transport.stop()

    def test_stale_or_empty_pong_never_verifies(self) -> None:
        for label, fake in (
            ("stale", FakeSerial(stale_input=b"FOF_PONG:stale\n")),
            ("empty", FakeSerial([b"FOF_PONG:\n"])),
        ):
            with self.subTest(label=label):
                updates: list[ConnectionUpdate] = []
                transport = BadgeSerialTransport(
                    enumerate_ports=lambda: [espressif("101")],
                    serial_factory=lambda fake=fake: fake,
                    monotonic_clock=StepClock(step=0.5),
                    on_connection=updates.append,
                )
                transport.start()
                self.assertTrue(fake.closed.wait(1.0), updates)
                transport.stop()
                self.assertNotIn("live", [update.state for update in updates])
                self.assertEqual(updates[-1].detail, "wrong_device")

    def test_ready_timeout_and_boot_logs_are_ignored_before_valid_pong(self) -> None:
        fake = FakeSerial(
            [
                b"FOF_READY\n",
                b"FOF_TIMEOUT\nI (42) usb ready\n",
                b"FOF_PONG:v-safe\n",
            ]
        )
        updates: list[ConnectionUpdate] = []
        live = threading.Event()

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "live":
                live.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=on_connection,
        )
        transport.start()
        self.assertTrue(live.wait(1.0), updates)
        self.assertEqual(updates[-1].firmware_version, "v-safe")
        transport.stop()

    def test_handshake_deadline_closes_and_reports_wrong_device(self) -> None:
        clock = StepClock(step=0.5)
        fake = FakeSerial()
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        wrong_device = next(update for update in updates if update.detail == "wrong_device")
        self.assertEqual(wrong_device.state, "error")
        self.assertLessEqual(clock.value, 5.0)


    def test_pong_returned_after_deadline_never_verifies(self) -> None:
        fake = FakeSerial([b"FOF_PONG:too-late\n"])
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            monotonic_clock=StepClock(step=2.0),
            on_connection=updates.append,
        )

        transport.start()
        deadline = time.monotonic() + 1.0
        while not fake.closed.is_set() and not any(
            update.state == "live" for update in updates
        ):
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])
        self.assertEqual(updates[-1].detail, "wrong_device")

    def test_stop_during_read_rejects_the_returned_pong(self) -> None:
        fake = FakeSerial([b"FOF_PONG:after-stop\n"], block_reads=True)
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(fake.read_started.wait(1.0), updates)
        with self.assertRaises(TransportError) as raised:
            transport.stop(timeout=0.0)
        self.assertEqual(raised.exception.code, "stop_timeout")
        fake.read_release.set()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])

    def test_stop_after_await_return_never_reaches_live_publication(self) -> None:
        await_returned = threading.Event()
        publish_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _await_pong(self, *args, **kwargs):  # type: ignore[no-untyped-def]
                result = super()._await_pong(*args, **kwargs)
                await_returned.set()
                publish_release.wait(1.0)
                return result

        fake = FakeSerial([b"FOF_PONG:before-stop\n"])
        updates: list[ConnectionUpdate] = []
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(await_returned.wait(1.0), updates)
        with self.assertRaises(TransportError):
            transport.stop(timeout=0.0)
        publish_release.set()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])

    def test_stop_after_generation_activation_never_reaches_live_publication(self) -> None:
        activated = threading.Event()
        publish_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _activate_generation(self) -> int:
                generation = super()._activate_generation()
                activated.set()
                publish_release.wait(1.0)
                return generation

        fake = FakeSerial([b"FOF_PONG:before-stop\n"])
        updates: list[ConnectionUpdate] = []
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(activated.wait(1.0), updates)
        with self.assertRaises(TransportError):
            transport.stop(timeout=0.0)
        publish_release.set()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])
        with self.assertRaises(TransportUnavailable):
            transport.send_control(build_display_nav("next"))

    def test_stop_immediately_before_live_callback_suppresses_publication(self) -> None:
        about_to_publish = threading.Event()
        publish_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _emit_session_update(self, state, *args, **kwargs):  # type: ignore[no-untyped-def]
                if state == "live":
                    about_to_publish.set()
                    publish_release.wait(1.0)
                return super()._emit_session_update(state, *args, **kwargs)

        fake = FakeSerial([b"FOF_PONG:before-stop\n"])
        updates: list[ConnectionUpdate] = []
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(about_to_publish.wait(1.0), updates)
        with self.assertRaises(TransportError):
            transport.stop(timeout=0.0)
        publish_release.set()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])

    def test_deadline_after_await_return_never_reaches_live_publication(self) -> None:
        await_returned = threading.Event()
        publish_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _await_pong(self, *args, **kwargs):  # type: ignore[no-untyped-def]
                result = super()._await_pong(*args, **kwargs)
                await_returned.set()
                publish_release.wait(1.0)
                return result

        clock = StepClock(step=0.0)
        fake = FakeSerial([b"FOF_PONG:before-deadline\n"])
        updates: list[ConnectionUpdate] = []
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(await_returned.wait(1.0), updates)
        clock.value = 4.0
        publish_release.set()
        deadline = time.monotonic() + 1.0
        while not fake.closed.is_set() and not any(
            update.state == "live" for update in updates
        ):
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertNotIn("live", [update.state for update in updates])
        self.assertEqual(updates[-1].detail, "wrong_device")

    def test_explicit_path_still_requires_application_pong(self) -> None:
        explicit = PortIdentity("/dev/cu.manual", None, None, None, None, None, None)
        for incoming, expected_live in (([b"FOF_PONG:manual\n"], True), ([], False)):
            with self.subTest(expected_live=expected_live):
                fake = FakeSerial(incoming)
                updates: list[ConnectionUpdate] = []
                transport = BadgeSerialTransport(
                    explicit_port=explicit.device,
                    enumerate_ports=lambda: [explicit],
                    serial_factory=lambda fake=fake: fake,
                    monotonic_clock=(
                        time.monotonic if expected_live else StepClock(step=0.5)
                    ),
                    on_connection=updates.append,
                )
                transport.start()
                if expected_live:
                    deadline = time.monotonic() + 1.0
                    while not any(update.state == "live" for update in updates):
                        self.assertLess(time.monotonic(), deadline, updates)
                        time.sleep(0.001)
                else:
                    self.assertTrue(fake.closed.wait(1.0), updates)
                transport.stop()
                self.assertEqual(
                    any(update.state == "live" for update in updates), expected_live
                )

    def test_unsupported_exclusive_attribute_preserves_safe_open_sequence(self) -> None:
        fake = FakeSerial([b"FOF_PONG:v1\n"], exclusive_supported=False)
        live = threading.Event()
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=lambda update: live.set() if update.state == "live" else None,
        )

        transport.start()
        self.assertTrue(live.wait(1.0), fake.actions)
        control_line_actions = [
            action
            for action in fake.actions
            if action[0] in {"setDTR", "setRTS"}
            or action[:2] in {("set", "dtr"), ("set", "rts")}
        ]
        self.assertEqual(control_line_actions, [])
        transport.stop()

    def test_read_failure_closes_and_reports_typed_detail(self) -> None:
        fake = FakeSerial(read_exception=OSError("device vanished"))
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self.assertTrue(fake.closed.wait(1.0), updates)
        transport.stop()

        read_error = next(update for update in updates if update.detail == "read_error")
        self.assertEqual(read_error.state, "error")

    def test_open_failure_closes_the_constructed_handle(self) -> None:
        fake = FakeSerial(open_exception=OSError("port busy"))
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        deadline = time.monotonic() + 1.0
        while not updates or updates[-1].detail != "open_error":
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertTrue(fake.closed.is_set())
        self.assertTrue(
            any(update.state == "error" and update.detail == "open_error" for update in updates)
        )


class PollingTest(unittest.TestCase):
    def test_status_is_immediate_periodic_and_limited_to_one_catch_up(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write_count(fake, 2)
        self.assertEqual(fake.writes, [b"\nFOF_PING\n", b"FOF_STATUS\n"])

        clock.advance(1.999)
        self.assertFalse(self._write_count_reached(fake, 3, timeout=0.02))
        clock.advance(0.001)
        self._wait_for_write_count(fake, 3)
        self.assertEqual(fake.writes[-1], b"FOF_STATUS\n")

        clock.advance(5.0)
        self._wait_for_write_count(fake, 4)
        self.assertFalse(self._write_count_reached(fake, 5, timeout=0.02))
        clock.advance(1.0)
        self._wait_for_write_count(fake, 5)
        transport.stop()

    def test_routes_data_frames_and_counts_bad_machine_input(self) -> None:
        monotonic = ManualClock()
        wall = ManualClock(start=1_700_000_000.0)
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        frames: list[tuple[object, float]] = []
        updates: list[ConnectionUpdate] = []
        frame_received = threading.Event()

        def on_frame(frame: object, received_at: float) -> None:
            frames.append((frame, received_at))
            if len(frames) == 2:
                frame_received.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            wall_clock=wall,
            monotonic_clock=monotonic,
            on_frame=on_frame,
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write_count(fake, 2)
        fake.feed(
            b"I (42) ordinary firmware log\n",
            b"FOF_DET:{bad-json}\n",
            b'FOF_DET:{"id":"det-1","source":3}\n',
            b"x" * 65_537
            + b'\nFOF_STATUS:{"version":"v-live","uptime_s":7}\n',
        )
        self.assertTrue(frame_received.wait(1.0), (frames, updates))
        transport.stop()

        self.assertEqual([frame.kind for frame, _ in frames], ["detection", "status"])
        self.assertEqual([received_at for _, received_at in frames], [wall.value] * 2)
        self.assertTrue(any(update.malformed_frames == 1 for update in updates))
        self.assertTrue(any(update.overlong_lines == 1 for update in updates))
        self.assertTrue(any(update.state == "live" for update in updates))

    def test_numeric_and_depth_failures_do_not_churn_the_live_session(self) -> None:
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        frames: list[object] = []
        updates: list[ConnectionUpdate] = []
        valid_frame = threading.Event()
        huge_integer = b"9" * 400
        deep_value = b"[" * 500 + b"0" + b"]" * 500

        def on_frame(frame: object, _received_at: float) -> None:
            frames.append(frame)
            valid_frame.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            on_frame=on_frame,
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write_count(fake, 2)
        fake.feed(
            b'FOF_STATUS:{"version":"v1","uptime_s":'
            + huge_integer
            + b',"entities":[]}\n',
            b'FOF_STATUS:{"version":"v1","uptime_s":1e309,"entities":[]}\n',
            b'FOF_STATUS:{"version":"v1","uptime_s":0,"future":'
            + deep_value
            + b'}\n',
            b'FOF_DET:{"id":"still-live","source":3}\n',
        )

        self.assertTrue(valid_frame.wait(1.0), (frames, updates))
        transport.stop()

        self.assertEqual([frame.kind for frame in frames], ["detection"])
        self.assertFalse(any(update.detail == "serial_error" for update in updates))
        self.assertTrue(any(update.malformed_frames == 3 for update in updates))

    def _wait_for_write_count(self, fake: FakeSerial, count: int) -> None:
        self.assertTrue(self._write_count_reached(fake, count), fake.writes)

    def _write_count_reached(
        self,
        fake: FakeSerial,
        count: int,
        *,
        timeout: float = 1.0,
    ) -> bool:
        deadline = time.monotonic() + timeout
        while len(fake.writes) < count and time.monotonic() < deadline:
            fake.written.wait(0.001)
            fake.written.clear()
        return len(fake.writes) >= count


class LiteUsbParityTest(unittest.TestCase):
    def test_exact_lite_identity_starts_live_acks_once_and_keeps_detections(self) -> None:
        fake = FakeSerial([b"FOF_PONG:0.2.0-backend\n"])
        frames: list[object] = []
        detection_seen = threading.Event()

        def on_frame(frame: object, _received_at: float) -> None:
            frames.append(frame)
            if getattr(frame, "kind", None) == "detection":
                detection_seen.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="LITE")],
            serial_factory=lambda: fake,
            on_frame=on_frame,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        fake.feed(lite_status())
        self._wait_for_write(
            fake, b'FOF_LIVE_START:{"client":"new_dash","protocol":1}\n'
        )
        fake.feed(
            b'FOF_LIVE_READY:{"session_id":"boot-a1","heartbeat_ms":5000,"lease_ms":15000}\n',
            b'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":1}\n',
            b'FOF_DET:{"id":"det-lite","source":3,"badge_class":"drone"}\n',
        )
        ack = b'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":1}\n'
        self._wait_for_write(fake, ack)
        self.assertTrue(detection_seen.wait(1.0), frames)

        fake.feed(
            b'FOF_LIVE_HEARTBEAT:{"session_id":"wrong","sequence":2}\n',
            b'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":1}\n',
        )
        time.sleep(0.02)
        self.assertEqual(fake.writes.count(ack), 1)
        self.assertEqual(
            [frame.kind for frame in frames],
            ["status", "detection"],
        )

        transport.stop()
        self.assertIn(
            b'FOF_LIVE_STOP:{"session_id":"boot-a1"}\n',
            fake.writes,
        )

    def test_lite_tuple_or_version_mismatch_never_starts_acknowledged_live(self) -> None:
        for label, status in (
            ("version", lite_status(version="different")),
            ("hardware", lite_status(hardware="other_s3")),
            ("project", lite_status(project="fof_badge_uplink")),
        ):
            with self.subTest(label=label):
                fake = FakeSerial([b"FOF_PONG:0.2.0-backend\n"])
                transport = BadgeSerialTransport(
                    enumerate_ports=lambda: [espressif("101", serial_number="LITE")],
                    serial_factory=lambda: fake,
                )
                transport.start()
                self._wait_for_write(fake, b"FOF_STATUS\n")
                fake.feed(status)
                self.assertTrue(fake.closed.wait(1.0), fake.writes)
                transport.stop()
                self.assertFalse(any(wire.startswith(b"FOF_LIVE_START:") for wire in fake.writes))

    def test_missing_heartbeat_for_one_lease_sends_fresh_start_not_replayed_ack(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:0.2.0-backend\n"])
        status_count = 0
        status_lock = threading.Lock()

        def on_frame(frame: object, _received_at: float) -> None:
            nonlocal status_count
            if getattr(frame, "kind", None) == "status":
                with status_lock:
                    status_count += 1

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="LITE")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_frame=on_frame,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        fake.feed(lite_status())
        start = b'FOF_LIVE_START:{"client":"new_dash","protocol":1}\n'
        self._wait_for_write(fake, start)
        for elapsed, expected_count in ((10.0, 2), (4.0, 3)):
            clock.advance(elapsed)
            fake.feed(lite_status())
            deadline = time.monotonic() + 1.0
            while True:
                with status_lock:
                    if status_count >= expected_count:
                        break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.001)
        clock.advance(1.0)
        deadline = time.monotonic() + 1.0
        while fake.writes.count(start) < 2:
            self.assertLess(time.monotonic(), deadline, fake.writes)
            time.sleep(0.001)
        self.assertFalse(any(wire.startswith(b"FOF_LIVE_ACK:") for wire in fake.writes))

    def _wait_for_write(self, fake: FakeSerial, expected: bytes) -> None:
        deadline = time.monotonic() + 1.0
        while expected not in fake.writes and time.monotonic() < deadline:
            fake.written.wait(0.001)
            fake.written.clear()
        self.assertIn(expected, fake.writes)


class ControlTest(unittest.TestCase):
    THEME = {
        "version": 1,
        "palette": "night",
        "background": "dark",
        "brightness": 80,
        "accents": {
            "drone": 65184,
            "meta": 63539,
            "tracker": 63519,
            "flock": 43039,
            "wifi_attack": 2047,
            "clear": 12133,
        },
    }

    def test_pong_only_generation_rejects_requests_until_fresh_native_status(self) -> None:
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        command = build_display_nav("next")

        with self.assertRaises(TransportUnavailable):
            transport.send_control(command)
        with self.assertRaises(UnsupportedCapability):
            transport.get_lite_config()
        self.assertNotIn(command.to_wire(), fake.writes)
        self.assertNotIn(b"FOF_CONFIG_GET\n", fake.writes)

        self._authorize_native(transport, fake)
        replies: list[object] = []
        caller = threading.Thread(
            target=lambda: replies.append(transport.send_control(command))
        )
        caller.start()
        self._wait_for_write(fake, command.to_wire())
        fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        caller.join(1.0)

        self.assertFalse(caller.is_alive())
        self.assertTrue(replies[0].ok)  # type: ignore[union-attr]

    def test_serializes_callers_and_correlates_exact_success_message(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        frames: list[object] = []
        status_seen = threading.Event()
        nav = build_display_nav("next")
        theme = build_theme(self.THEME)
        results: dict[str, object] = {}

        def on_frame(frame: object, _received_at: float) -> None:
            frames.append(frame)
            status_seen.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_frame=on_frame,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)

        first = threading.Thread(
            target=lambda: results.setdefault("nav", transport.send_control(nav)),
        )
        first.start()
        self._wait_for_write(fake, nav.to_wire())
        second = threading.Thread(
            target=lambda: results.setdefault("theme", transport.send_control(theme)),
        )
        second.start()
        self.assertNotIn(theme.to_wire(), fake.writes)

        fake.feed(
            b'FOF_STATUS:{"version":"v-live","uptime_s":1}\n',
            b'FOF_CTL_OK:{"message":"badge theme updated"}\n',
        )
        self.assertTrue(status_seen.wait(1.0), frames)
        first.join(0.02)
        self.assertTrue(first.is_alive())
        self.assertNotIn(theme.to_wire(), fake.writes)

        fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        first.join(1.0)
        self.assertFalse(first.is_alive())
        self._wait_for_write(fake, theme.to_wire())
        fake.feed(
            b'FOF_CTL_OK:{"message":"badge theme updated","ble_sent":false}\n'
        )
        second.join(1.0)
        self.assertFalse(second.is_alive())
        transport.stop()

        self.assertTrue(results["nav"].ok)  # type: ignore[union-attr]
        self.assertFalse(results["theme"].details["ble_sent"])  # type: ignore[union-attr]
        self.assertEqual(fake.write_threads, ["new-dash-serial"] * len(fake.writes))

    def test_error_reply_is_returned_to_the_outstanding_caller(self) -> None:
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)
        command = build_display_nav("back")
        result: list[object] = []
        caller = threading.Thread(target=lambda: result.append(transport.send_control(command)))
        caller.start()
        self._wait_for_write(fake, command.to_wire())
        fake.feed(b'FOF_CTL_ERROR:{"error":"rejected","message":"no"}\n')
        caller.join(1.0)
        self.assertFalse(caller.is_alive())
        self.assertFalse(result[0].ok)  # type: ignore[union-attr]
        self.assertEqual(result[0].error, "rejected")  # type: ignore[union-attr]

    def test_timeout_quarantines_generation_and_fails_queued_mutations(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)
        first_errors: list[BaseException] = []
        queued_errors: list[BaseException] = []

        def call(command: object, errors: list[BaseException]) -> None:
            try:
                transport.send_control(command)  # type: ignore[arg-type]
            except BaseException as error:
                errors.append(error)

        first = threading.Thread(
            target=call,
            args=(build_display_nav("next"), first_errors),
        )
        first.start()
        self._wait_for_write(fake, build_display_nav("next").to_wire())
        queued = threading.Thread(
            target=call,
            args=(build_display_nav("detail"), queued_errors),
        )
        queued.start()
        clock.advance(5.0)
        first.join(1.0)
        queued.join(1.0)

        self.assertIsInstance(first_errors[0], ControlTimeout)
        self.assertIsInstance(queued_errors[0], TransportUnavailable)
        self.assertTrue(fake.closed.wait(1.0))
        with self.assertRaises(TransportUnavailable):
            transport.send_control(build_display_nav("page"))

    def test_queued_control_receives_full_timeout_window_after_transmission(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)
        first_command = build_display_nav("next")
        second_command = build_display_nav("detail")
        replies: dict[str, object] = {}
        errors: list[BaseException] = []

        def call(name: str, command: object) -> None:
            try:
                replies[name] = transport.send_control(command)  # type: ignore[arg-type]
            except BaseException as error:
                errors.append(error)

        first = threading.Thread(target=call, args=("first", first_command))
        second = threading.Thread(target=call, args=("second", second_command))
        first.start()
        self._wait_for_write(fake, first_command.to_wire())
        second.start()
        clock.advance(0.2)
        self.assertFalse(fake.closed.wait(0.02))
        clock.advance(4.7)
        fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        self._wait_for_write(fake, second_command.to_wire())

        clock.advance(0.2)
        self.assertFalse(fake.closed.wait(0.02))
        clock.advance(4.7)
        fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        first.join(1.0)
        second.join(1.0)

        self.assertEqual(errors, [])
        self.assertTrue(replies["first"].ok)  # type: ignore[union-attr]
        self.assertTrue(replies["second"].ok)  # type: ignore[union-attr]

    def test_ack_at_deadline_times_out_and_quarantines_generation(self) -> None:
        clock = ManualClock()

        class DeadlineAckSerial(FakeSerial):
            def read(self, size: int = 1) -> bytes:
                data = super().read(size)
                if data.startswith(b"FOF_CTL_OK:"):
                    clock.advance(5.0)
                return data

        fake = DeadlineAckSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)
        command = build_display_nav("next")
        errors: list[BaseException] = []
        replies: list[object] = []

        def send() -> None:
            try:
                replies.append(transport.send_control(command))
            except BaseException as error:
                errors.append(error)

        caller = threading.Thread(target=send)
        caller.start()
        self._wait_for_write(fake, command.to_wire())
        fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        caller.join(1.0)

        self.assertEqual(replies, [])
        self.assertEqual(len(errors), 1)
        self.assertIsInstance(errors[0], ControlTimeout)
        self.assertTrue(fake.closed.wait(1.0))
        with self.assertRaises(TransportUnavailable):
            transport.send_control(build_display_nav("detail"))

    def test_disconnect_fails_pending_and_future_requests(self) -> None:
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)
        errors: list[BaseException] = []
        command = build_display_nav("next")

        def send() -> None:
            try:
                transport.send_control(command)
            except BaseException as error:
                errors.append(error)

        caller = threading.Thread(target=send)
        caller.start()
        self._wait_for_write(fake, command.to_wire())
        fake.read_exception = OSError("detached")
        caller.join(1.0)
        self.assertIsInstance(errors[0], TransportUnavailable)
        with self.assertRaises(TransportUnavailable):
            transport.send_control(command)

    def test_stop_after_dequeue_prevents_a_new_mutation_write(self) -> None:
        taken = threading.Event()
        write_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _take_next_control(self, generation):  # type: ignore[no-untyped-def]
                request = super()._take_next_control(generation)
                if request is not None:
                    taken.set()
                    write_release.wait(1.0)
                return request

        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
        )
        command = build_display_nav("next")
        errors: list[BaseException] = []
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)

        def send() -> None:
            try:
                transport.send_control(command)
            except BaseException as error:
                errors.append(error)

        caller = threading.Thread(target=send)
        caller.start()
        self.assertTrue(taken.wait(1.0))
        with self.assertRaises(TransportError):
            transport.stop(timeout=0.0)
        write_release.set()
        self.assertTrue(fake.closed.wait(1.0))
        caller.join(1.0)
        transport.stop()

        self.assertNotIn(command.to_wire(), fake.writes)
        self.assertIsInstance(errors[0], TransportUnavailable)

    def test_stop_immediately_before_control_write_suppresses_mutation(self) -> None:
        about_to_write = threading.Event()
        write_release = threading.Event()

        class PausingTransport(BadgeSerialTransport):
            def _write_if_running(self, serial_port, data, stop_event):  # type: ignore[no-untyped-def]
                if data.startswith(b"FOF_CTL:"):
                    about_to_write.set()
                    write_release.wait(1.0)
                return super()._write_if_running(serial_port, data, stop_event)

        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        transport = PausingTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
        )
        command = build_display_nav("next")
        errors: list[BaseException] = []
        transport.start()
        self._wait_for_write(fake, b"FOF_STATUS\n")
        self._authorize_native(transport, fake)

        def send() -> None:
            try:
                transport.send_control(command)
            except BaseException as error:
                errors.append(error)

        caller = threading.Thread(target=send)
        caller.start()
        reached_write_boundary = about_to_write.wait(0.05)
        if not reached_write_boundary:
            transport.stop()
            caller.join(1.0)
            self.fail("control write did not use the stop-linearized boundary")
        with self.assertRaises(TransportError):
            transport.stop(timeout=0.0)
        write_release.set()
        self.assertTrue(fake.closed.wait(1.0))
        caller.join(1.0)
        transport.stop()

        self.assertNotIn(command.to_wire(), fake.writes)
        self.assertIsInstance(errors[0], TransportUnavailable)

    def _wait_for_write(self, fake: FakeSerial, expected: bytes) -> None:
        deadline = time.monotonic() + 1.0
        while expected not in fake.writes and time.monotonic() < deadline:
            fake.written.wait(0.001)
            fake.written.clear()
        self.assertIn(expected, fake.writes)

    def _authorize_native(
        self,
        transport: BadgeSerialTransport,
        fake: FakeSerial,
    ) -> None:
        fake.feed(native_status())
        deadline = time.monotonic() + 1.0
        while transport._verified_family != "regular_badge" and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertEqual(transport._verified_family, "regular_badge")


class ReconnectTest(unittest.TestCase):
    class RecordingRetryTransport(BadgeSerialTransport):
        def __init__(self, *args, stop_after_waits: int | None = None, **kwargs):  # type: ignore[no-untyped-def]
            super().__init__(*args, **kwargs)
            self.delays: list[float] = []
            self.stop_after_waits = stop_after_waits

        def _wait_before_retry(self, stop_event, delay):  # type: ignore[no-untyped-def]
            self.delays.append(delay)
            if self.stop_after_waits is not None and len(self.delays) >= self.stop_after_waits:
                stop_event.set()
            return stop_event.is_set()

    def test_status_becomes_stale_at_six_seconds_without_deleting_snapshot(self) -> None:
        monotonic = ManualClock()
        wall = ManualClock(start=1_700_000_000.0)
        fake = FakeSerial(
            [
                b"FOF_PONG:v-live\n",
                b'FOF_STATUS:{"version":"v-live","uptime_s":1}\n',
            ]
        )
        frames: list[object] = []
        updates: list[ConnectionUpdate] = []
        stale = threading.Event()

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "stale":
                stale.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=monotonic,
            wall_clock=wall,
            on_frame=lambda frame, _received_at: frames.append(frame),
            on_connection=on_connection,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(lambda: len(frames) == 1)
        monotonic.advance(5.999)
        self.assertFalse(stale.wait(0.02))
        monotonic.advance(0.001)
        self.assertTrue(stale.wait(1.0), updates)
        transport.stop()

        self.assertEqual(len(frames), 1)
        stale_update = next(update for update in updates if update.state == "stale")
        self.assertEqual(stale_update.last_valid_status_at, wall.value)

    def test_valid_detection_refreshes_session_liveness_but_not_status_age(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        updates: list[ConnectionUpdate] = []
        detection_seen = threading.Event()
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_frame=lambda frame, _received_at: (
                detection_seen.set() if frame.kind == "detection" else None
            ),
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(lambda: any(update.state == "live" for update in updates))
        clock.advance(11.0)
        fake.feed(b'FOF_DET:{"id":"still-live","source":3}\n')
        self.assertTrue(detection_seen.wait(1.0))
        self._wait_for(lambda: any(update.state == "stale" for update in updates))
        clock.advance(11.999)
        self.assertFalse(fake.closed.wait(0.02))
        clock.advance(0.001)
        self.assertTrue(fake.closed.wait(1.0), updates)

    def test_twelve_seconds_of_machine_silence_closes_and_reconnects(self) -> None:
        clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        updates: list[ConnectionUpdate] = []
        reconnecting = threading.Event()

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "reconnecting":
                reconnecting.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_connection=on_connection,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(lambda: any(update.state == "live" for update in updates))
        clock.advance(12.0)
        self.assertTrue(fake.closed.wait(1.0), updates)
        self.assertTrue(reconnecting.wait(1.0), updates)

    def test_path_disappearance_closes_current_handle_immediately(self) -> None:
        present = True
        clock = ManualClock()
        identity = espressif("101", serial_number="ABC")
        fake = FakeSerial([b"FOF_PONG:v-live\n"])
        live = threading.Event()
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [identity] if present else [],
            serial_factory=lambda: fake,
            monotonic_clock=clock,
            on_connection=lambda update: live.set() if update.state == "live" else None,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self.assertTrue(live.wait(1.0))
        present = False
        clock.advance(1.0)
        self.assertTrue(fake.closed.wait(1.0))

    def test_retry_backoff_caps_at_ten_seconds(self) -> None:
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [],
            serial_factory=lambda: self.fail("no port should be opened"),
            stop_after_waits=6,
        )
        transport.start()
        worker = transport._worker
        self.assertIsNotNone(worker)
        worker.join(1.0)  # type: ignore[union-attr]
        transport.stop()
        self.assertEqual(transport.delays, [1.0, 2.0, 4.0, 8.0, 10.0, 10.0])

    def test_retry_backoff_remains_capped_after_one_thousand_attempts(self) -> None:
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [],
            serial_factory=lambda: self.fail("no port should be opened"),
            stop_after_waits=1_026,
        )
        transport.start()
        worker = transport._worker
        self.assertIsNotNone(worker)
        worker.join(2.0)  # type: ignore[union-attr]
        transport.stop()

        self.assertEqual(len(transport.delays), 1_026)
        self.assertEqual(transport.delays[-1], 10.0)

    def test_verified_pong_resets_backoff_to_one_second(self) -> None:
        fakes = iter(
            (
                FakeSerial(open_exception=OSError("busy one")),
                FakeSerial(open_exception=OSError("busy two")),
                FakeSerial([b"FOF_PONG:v-live\n"], read_exception=OSError("gone")),
            )
        )
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [espressif("101", serial_number="ABC")],
            serial_factory=lambda: next(fakes),
            stop_after_waits=3,
        )
        transport.start()
        worker = transport._worker
        self.assertIsNotNone(worker)
        worker.join(1.0)  # type: ignore[union-attr]
        transport.stop()
        self.assertEqual(transport.delays, [1.0, 2.0, 1.0])

    def test_stop_interrupts_reconnect_wait_and_joins_worker(self) -> None:
        reconnecting = threading.Event()
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [],
            on_connection=lambda update: (
                reconnecting.set() if update.state == "reconnecting" else None
            ),
        )
        transport.start()
        self.assertTrue(reconnecting.wait(1.0))
        started = time.monotonic()
        transport.stop()
        self.assertLess(time.monotonic() - started, 0.5)

    def test_timeout_requires_fresh_pong_before_later_mutation(self) -> None:
        clock = ManualClock()
        identity = espressif("101", serial_number="ABC")
        first_fake = FakeSerial([b"FOF_PONG:first\n"])
        second_fake = FakeSerial()
        fakes = iter((first_fake, second_fake))
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [identity],
            serial_factory=lambda: next(fakes),
            monotonic_clock=clock,
        )
        self.addCleanup(transport.stop)
        transport.start()
        first_error: list[BaseException] = []

        def first_call() -> None:
            try:
                transport.send_control(build_display_nav("next"))
            except BaseException as error:
                first_error.append(error)

        self._wait_for(lambda: b"FOF_STATUS\n" in first_fake.writes)
        first_fake.feed(native_status("first"))
        self._wait_for(lambda: transport._verified_family == "regular_badge")
        caller = threading.Thread(target=first_call)
        caller.start()
        self._wait_for(lambda: build_display_nav("next").to_wire() in first_fake.writes)
        clock.advance(5.0)
        caller.join(1.0)
        self.assertIsInstance(first_error[0], ControlTimeout)
        self.assertTrue(first_fake.closed.wait(1.0))
        self._wait_for(lambda: b"\nFOF_PING\n" in second_fake.writes)
        second_fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        with self.assertRaises(TransportUnavailable):
            transport.send_control(build_display_nav("next"))

        second_fake.feed(b"FOF_PONG:second\n")
        self._wait_for(lambda: second_fake.writes.count(b"FOF_STATUS\n") == 1)
        with self.assertRaises(TransportUnavailable):
            transport.send_control(build_display_nav("next"))
        second_fake.feed(native_status("second"))
        self._wait_for(lambda: transport._verified_family == "regular_badge")
        result: list[object] = []
        next_caller = threading.Thread(
            target=lambda: result.append(transport.send_control(build_display_nav("next")))
        )
        next_caller.start()
        self._wait_for(lambda: build_display_nav("next").to_wire() in second_fake.writes)
        second_fake.feed(b'FOF_CTL_OK:{"message":"display nav updated"}\n')
        next_caller.join(1.0)
        self.assertTrue(result[0].ok)  # type: ignore[union-attr]

    def test_reconnect_follows_remembered_identity_to_changed_path(self) -> None:
        first = espressif("101", serial_number="ABC", location="1-1")
        moved = espressif("202", serial_number="ABC", location="2-4")
        other = espressif("303", serial_number="OTHER", location="3-1")
        enumerations = iter(([first], [other, moved]))
        first_fake = FakeSerial(
            [b"FOF_PONG:first\n"], read_exception=OSError("detached")
        )
        second_fake = FakeSerial([b"FOF_PONG:second\n"])
        fakes = iter((first_fake, second_fake))
        updates: list[ConnectionUpdate] = []
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: next(enumerations),
            serial_factory=lambda: next(fakes),
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(
            lambda: any(update.firmware_version == "second" for update in updates)
        )
        transport.stop()
        second_connecting = next(
            update
            for update in updates
            if update.state == "connecting" and update.port == moved.device
        )
        self.assertEqual(second_connecting.port, moved.device)

    def test_reconnect_retains_status_time_and_cumulative_parser_counters(self) -> None:
        wall = ManualClock(start=1_700_000_000.0)
        identity = espressif("101", serial_number="ABC")
        first_fake = FakeSerial(
            [
                b"FOF_PONG:first\n",
                b'FOF_STATUS:{"version":"first","uptime_s":1}\n',
                b"FOF_DET:{bad-json}\n",
                b"x" * 65_537 + b"\n",
            ],
            read_exception=OSError("detached"),
        )
        second_fake = FakeSerial([b"FOF_PONG:second\n"])
        fakes = iter((first_fake, second_fake))
        updates: list[ConnectionUpdate] = []
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [identity],
            serial_factory=lambda: next(fakes),
            wall_clock=wall,
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(
            lambda: any(update.firmware_version == "second" for update in updates)
        )
        transport.stop()

        second_live = next(
            update
            for update in updates
            if update.state == "live" and update.firmware_version == "second"
        )
        self.assertEqual(second_live.last_valid_status_at, wall.value)
        self.assertEqual(second_live.malformed_frames, 1)
        self.assertEqual(second_live.overlong_lines, 1)

    def test_old_status_remains_stale_after_reconnect_until_new_status(self) -> None:
        clock = ManualClock()
        wall = ManualClock(start=1_700_000_000.0)
        identity = espressif("101", serial_number="ABC")
        first_fake = FakeSerial(
            [
                b"FOF_PONG:first\n",
                b'FOF_STATUS:{"version":"first","uptime_s":1}\n',
            ],
            read_exception=OSError("detached"),
        )
        second_fake = FakeSerial([b"FOF_PONG:second\n"])
        fakes = iter((first_fake, second_fake))
        updates: list[ConnectionUpdate] = []
        second_stale = threading.Event()
        recovered = threading.Event()

        class AgingRetryTransport(self.RecordingRetryTransport):
            def _wait_before_retry(self, stop_event, delay):  # type: ignore[no-untyped-def]
                self.delays.append(delay)
                clock.advance(7.0)
                return stop_event.is_set()

        def on_connection(update: ConnectionUpdate) -> None:
            updates.append(update)
            if update.state == "stale" and update.firmware_version == "second":
                second_stale.set()
            if update.detail == "status_recovered":
                recovered.set()

        transport = AgingRetryTransport(
            enumerate_ports=lambda: [identity],
            serial_factory=lambda: next(fakes),
            monotonic_clock=clock,
            wall_clock=wall,
            on_connection=on_connection,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self.assertTrue(second_stale.wait(1.0), updates)
        self.assertEqual(updates[-1].last_valid_status_at, wall.value)
        self.assertFalse(
            any(
                update.state == "live" and update.firmware_version == "second"
                for update in updates
            )
        )

        second_fake.feed(b'FOF_STATUS:{"version":"second","uptime_s":2}\n')
        self.assertTrue(recovered.wait(1.0), updates)
        transport.stop()

    def test_close_failure_cannot_terminate_reconnect_owner(self) -> None:
        identity = espressif("101", serial_number="ABC")

        class CloseRaisingSerial(FakeSerial):
            def close(self) -> None:
                super().close()
                raise OSError("close failed")

        first_fake = CloseRaisingSerial(
            [b"FOF_PONG:first\n"],
            read_exception=OSError("detached"),
        )
        second_fake = FakeSerial([b"FOF_PONG:second\n"])
        fakes = iter((first_fake, second_fake))
        updates: list[ConnectionUpdate] = []
        transport = self.RecordingRetryTransport(
            enumerate_ports=lambda: [identity],
            serial_factory=lambda: next(fakes),
            on_connection=updates.append,
        )
        self.addCleanup(transport.stop)
        transport.start()
        self._wait_for(
            lambda: any(update.firmware_version == "second" for update in updates)
        )
        transport.stop()

        self.assertTrue(first_fake.closed.is_set())
        self.assertTrue(any(update.state == "reconnecting" for update in updates))

    def _wait_for(self, predicate) -> None:  # type: ignore[no-untyped-def]
        deadline = time.monotonic() + 1.0
        while not predicate() and time.monotonic() < deadline:
            threading.Event().wait(0.001)
        self.assertTrue(predicate())


class RememberedIdentityTest(unittest.TestCase):
    def test_serial_number_follows_a_verified_badge_to_its_new_path(self) -> None:
        first = espressif("101", serial_number="ABC", location="1-1")
        moved = espressif("202", serial_number="ABC", location="2-4")
        other = espressif("303", serial_number="OTHER", location="3-1")

        selected = self._run_two_starts([first], [other, moved])

        self.assertEqual(selected, moved.device)

    def test_location_follows_a_verified_badge_when_serial_number_is_absent(self) -> None:
        first = espressif("101", location="1-1")
        moved = espressif("202", location="1-1")
        other = espressif("303", location="3-1")

        selected = self._run_two_starts([first], [other, moved])

        self.assertEqual(selected, moved.device)

    def test_remembered_serial_number_never_overrides_wrong_numeric_ids(self) -> None:
        first = espressif("101", serial_number="ABC")
        wrong_ids = PortIdentity(
            "/dev/cu.usbmodem202",
            ESPRESSIF_VID,
            0x9999,
            "Espressif",
            "USB JTAG/serial",
            "ABC",
            None,
        )
        other = espressif("303", serial_number="OTHER")
        enumerations = iter(([first], [wrong_ids, other]))
        fake = FakeSerial([b"FOF_PONG:first\n"])
        factory_calls = 0
        updates: list[ConnectionUpdate] = []

        def factory() -> FakeSerial:
            nonlocal factory_calls
            factory_calls += 1
            return fake

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: next(enumerations),
            serial_factory=factory,
            on_connection=updates.append,
        )
        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        deadline = time.monotonic() + 1.0
        while len(updates) < 3:
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertEqual(factory_calls, 1)
        self.assertEqual(updates[-1].detail, "no_badge")

    def test_missing_stable_identity_leaves_multiple_candidates_ambiguous(self) -> None:
        first = espressif("101")
        enumerations = iter(([first], [espressif("202"), espressif("303")]))
        fake = FakeSerial([b"FOF_PONG:first\n"])
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            enumerate_ports=lambda: next(enumerations),
            serial_factory=lambda: fake,
            on_connection=updates.append,
        )

        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        deadline = time.monotonic() + 1.0
        while updates[-1].detail != "multiple_badges":
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertTrue(
            any(
                update.state == "error" and update.detail == "multiple_badges"
                for update in updates
            )
        )

    def test_automatic_mode_without_stable_identity_waits_for_verified_path(self) -> None:
        first = espressif("101")
        changed = espressif("202")
        enumerations = iter(([first], [changed]))
        first_fake = FakeSerial([b"FOF_PONG:first\n"])
        factory_calls = 0
        updates: list[ConnectionUpdate] = []

        def factory() -> FakeSerial:
            nonlocal factory_calls
            factory_calls += 1
            return first_fake

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: next(enumerations),
            serial_factory=factory,
            on_connection=updates.append,
        )

        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        deadline = time.monotonic() + 1.0
        while updates[-1].detail != "no_badge":
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertEqual(factory_calls, 1)
        self.assertEqual(updates[-1].state, "reconnecting")
        self.assertEqual(updates[-1].candidates, ((changed.device,),))

    def test_automatic_remembered_path_rejects_reuse_by_wrong_ids(self) -> None:
        first = espressif("101")
        reused_by_other_device = PortIdentity(
            first.device,
            0x05AC,
            0x1234,
            "Not Espressif",
            "Different serial device",
            None,
            None,
        )
        enumerations = iter(([first], [reused_by_other_device]))
        fakes = iter(
            (
                FakeSerial([b"FOF_PONG:first\n"]),
                FakeSerial([b"FOF_PONG:impostor\n"]),
            )
        )
        factory_calls = 0
        updates: list[ConnectionUpdate] = []

        def factory() -> FakeSerial:
            nonlocal factory_calls
            factory_calls += 1
            return next(fakes)

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: next(enumerations),
            serial_factory=factory,
            on_connection=updates.append,
        )

        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        deadline = time.monotonic() + 1.0
        while updates[-1].detail != "no_badge" and not any(
            update.firmware_version == "impostor" for update in updates
        ):
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertEqual(factory_calls, 1)
        self.assertEqual(updates[-1].detail, "no_badge")
        self.assertFalse(any(update.firmware_version == "impostor" for update in updates))

    def test_explicit_metadata_less_path_remains_eligible_after_verification(self) -> None:
        explicit = PortIdentity(
            "/dev/cu.manual",
            None,
            None,
            None,
            None,
            None,
            None,
        )

        selected = self._run_two_starts(
            [explicit],
            [explicit],
            explicit_port=explicit.device,
        )

        self.assertEqual(selected, explicit.device)

    def test_explicit_mode_follows_only_a_learned_stable_identity(self) -> None:
        first = espressif("101", serial_number="ABC")
        moved = espressif("202", serial_number="ABC")
        other = espressif("303", serial_number="OTHER")

        selected = self._run_two_starts(
            [first],
            [other, moved],
            explicit_port=first.device,
        )

        self.assertEqual(selected, moved.device)

    def test_explicit_mode_without_stable_identity_waits_for_original_path(self) -> None:
        first = espressif("101")
        enumerations = iter(([first], [espressif("202")]))
        first_fake = FakeSerial([b"FOF_PONG:first\n"])
        factory_calls = 0
        updates: list[ConnectionUpdate] = []

        def factory() -> FakeSerial:
            nonlocal factory_calls
            factory_calls += 1
            return first_fake

        transport = BadgeSerialTransport(
            explicit_port=first.device,
            enumerate_ports=lambda: next(enumerations),
            serial_factory=factory,
            on_connection=updates.append,
        )

        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        deadline = time.monotonic() + 1.0
        while updates[-1].detail != "explicit_port_missing":
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()

        self.assertEqual(factory_calls, 1)
        self.assertEqual(updates[-1].state, "reconnecting")

    def _run_two_starts(
        self,
        first_ports: list[PortIdentity],
        second_ports: list[PortIdentity],
        *,
        explicit_port: str | None = None,
    ) -> str:
        enumerations = iter((first_ports, second_ports))
        fakes = iter(
            (
                FakeSerial([b"FOF_PONG:first\n"]),
                FakeSerial([b"FOF_PONG:second\n"]),
            )
        )
        updates: list[ConnectionUpdate] = []
        transport = BadgeSerialTransport(
            explicit_port=explicit_port,
            enumerate_ports=lambda: next(enumerations),
            serial_factory=lambda: next(fakes),
            on_connection=updates.append,
        )

        transport.start()
        self._wait_for_live(updates, "first")
        transport.stop()
        transport.start()
        second_live = self._wait_for_live(updates, "second")
        transport.stop()
        return second_live.port or ""

    def _wait_for_live(
        self,
        updates: list[ConnectionUpdate],
        firmware_version: str,
    ) -> ConnectionUpdate:
        deadline = time.monotonic() + 1.0
        while True:
            for update in updates:
                if update.state == "live" and update.firmware_version == firmware_version:
                    return update
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)


class LifecycleConcurrencyTest(unittest.TestCase):
    def test_stop_timeout_bounds_an_already_active_callback(self) -> None:
        callback_active = threading.Event()
        callback_release = threading.Event()
        fake = FakeSerial([b"FOF_PONG:active-callback\n"])

        def on_connection(update: ConnectionUpdate) -> None:
            if update.state == "live":
                callback_active.set()
                callback_release.wait(1.0)

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=on_connection,
        )
        transport.start()
        self.assertTrue(callback_active.wait(1.0))
        stop_done = threading.Event()
        stop_errors: list[BaseException] = []

        def stop_transport() -> None:
            try:
                transport.stop(timeout=0.02)
            except BaseException as error:
                stop_errors.append(error)
            finally:
                stop_done.set()

        stopper = threading.Thread(target=stop_transport)
        started_at = time.monotonic()
        stopper.start()
        returned_within_bound = stop_done.wait(0.2)
        elapsed = time.monotonic() - started_at
        callback_release.set()
        stopper.join(1.0)
        self.assertTrue(fake.closed.wait(1.0))
        transport.stop()

        self.assertTrue(returned_within_bound, elapsed)
        self.assertLess(elapsed, 0.2)
        self.assertEqual(len(stop_errors), 1)
        self.assertIsInstance(stop_errors[0], TransportError)
        self.assertEqual(stop_errors[0].code, "stop_timeout")  # type: ignore[union-attr]

    def test_callback_triggered_stop_never_waits_on_its_own_side_effect(self) -> None:
        callback_done = threading.Event()
        callback_errors: list[BaseException] = []
        holder: dict[str, BadgeSerialTransport] = {}
        fake = FakeSerial([b"FOF_PONG:self-stop\n"])

        def on_connection(update: ConnectionUpdate) -> None:
            if update.state != "live":
                return
            try:
                holder["transport"].stop(timeout=0.02)
            except BaseException as error:
                callback_errors.append(error)
            finally:
                callback_done.set()

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=on_connection,
        )
        holder["transport"] = transport
        transport.start()
        self.assertTrue(callback_done.wait(0.2), callback_errors)
        self.assertTrue(fake.closed.wait(1.0))
        transport.stop()

        self.assertEqual(callback_errors, [])

    def test_active_callback_finishes_within_budget_then_worker_fully_joins(self) -> None:
        callback_active = threading.Event()
        callback_release = threading.Event()
        shutdown_clock = ManualClock()
        fake = FakeSerial([b"FOF_PONG:drain-and-join\n"])

        def on_connection(update: ConnectionUpdate) -> None:
            if update.state == "live":
                callback_active.set()
                callback_release.wait(1.0)

        transport = BadgeSerialTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=lambda: fake,
            on_connection=on_connection,
        )
        transport._shutdown_clock = shutdown_clock
        transport.start()
        self.assertTrue(callback_active.wait(1.0))
        stop_event = transport._worker_stop
        worker = transport._worker
        self.assertIsNotNone(stop_event)
        self.assertIsNotNone(worker)
        original_join = worker.join  # type: ignore[union-attr]
        join_timeouts: list[float | None] = []

        def recording_join(timeout: float | None = None) -> None:
            join_timeouts.append(timeout)
            original_join(timeout)

        worker.join = recording_join  # type: ignore[method-assign,union-attr]
        stop_done = threading.Event()
        stop_errors: list[BaseException] = []

        def stop_transport() -> None:
            try:
                transport.stop(timeout=1.0)
            except BaseException as error:
                stop_errors.append(error)
            finally:
                stop_done.set()

        stopper = threading.Thread(target=stop_transport)
        stopper.start()
        stop_committed_while_callback_active = stop_event.wait(0.2)  # type: ignore[union-attr]
        shutdown_clock.advance(0.4)
        callback_release.set()
        self.assertTrue(stop_done.wait(1.0), stop_errors)
        stopper.join(1.0)

        self.assertTrue(stop_committed_while_callback_active)
        self.assertEqual(stop_errors, [])
        self.assertTrue(fake.closed.is_set())
        self.assertIsNone(transport._worker)
        self.assertEqual(len(join_timeouts), 1)
        self.assertAlmostEqual(join_timeouts[0], 0.6)  # type: ignore[arg-type]

    def test_start_racing_a_stop_cannot_leave_a_replacement_worker_live(self) -> None:
        class OneAttemptTransport(BadgeSerialTransport):
            def _wait_before_retry(self, stop_event, delay):  # type: ignore[no-untyped-def]
                del delay
                stop_event.set()
                return True

        first_fake = FakeSerial(block_reads=True)
        second_fake = FakeSerial([b"FOF_PONG:second\n"])
        fakes = iter((first_fake, second_fake))
        factory_calls = 0
        updates: list[ConnectionUpdate] = []
        clock = StepClock(step=1.0)

        def factory() -> FakeSerial:
            nonlocal factory_calls
            factory_calls += 1
            return next(fakes)

        transport = OneAttemptTransport(
            enumerate_ports=lambda: [espressif("101")],
            serial_factory=factory,
            monotonic_clock=clock,
            on_connection=updates.append,
        )
        transport.start()
        self.assertTrue(first_fake.read_started.wait(1.0), updates)
        old_worker = transport._worker
        self.assertIsNotNone(old_worker)
        first_fake.read_release.set()
        self.assertTrue(first_fake.closed.wait(1.0), updates)
        old_worker.join(1.0)  # type: ignore[union-attr]
        self.assertFalse(old_worker.is_alive())  # type: ignore[union-attr]

        original_join = old_worker.join  # type: ignore[union-attr]
        join_entered = threading.Event()
        join_release = threading.Event()

        def delayed_join(timeout: float | None = None) -> None:
            original_join(timeout)
            join_entered.set()
            join_release.wait(1.0)

        old_worker.join = delayed_join  # type: ignore[method-assign,union-attr]
        stop_errors: list[BaseException] = []
        stop_done = threading.Event()

        def stop_transport() -> None:
            try:
                transport.stop()
            except BaseException as error:
                stop_errors.append(error)
            finally:
                stop_done.set()

        stopper = threading.Thread(target=stop_transport)
        stopper.start()
        self.assertTrue(join_entered.wait(1.0))

        raced_start_error: TransportError | None = None
        try:
            transport.start()
        except TransportError as error:
            raced_start_error = error
        join_release.set()
        self.assertTrue(stop_done.wait(1.0), stop_errors)
        stopper.join(1.0)
        replacement_was_live = second_fake.is_open
        if raced_start_error is None:
            transport.stop()

        self.assertEqual(stop_errors, [])
        self.assertIsNotNone(raced_start_error)
        self.assertEqual(
            raced_start_error.code,  # type: ignore[union-attr]
            "lifecycle_busy",
        )
        self.assertFalse(replacement_was_live)
        self.assertEqual(factory_calls, 1)

        clock.step = 0.1
        transport.start()
        deadline = time.monotonic() + 1.0
        while not any(update.firmware_version == "second" for update in updates):
            self.assertLess(time.monotonic(), deadline, updates)
            time.sleep(0.001)
        transport.stop()


if __name__ == "__main__":
    unittest.main()
