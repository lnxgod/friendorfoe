import threading
import time
import unittest

from new_dash.serial_transport import (
    BadgeSerialTransport,
    ConnectionUpdate,
    DiscoveryError,
    PortIdentity,
    TransportError,
    choose_candidate,
    discover_badge_ports,
)

from tests.fakes import FakeSerial, StepClock


ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001


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
    def test_safe_configuration_precedes_open_and_ping_precedes_live(self) -> None:
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
        self.assertEqual(
            fake.actions[:11],
            [
                ("set", "port", "/dev/cu.usbmodem101"),
                ("set", "baudrate", 115200),
                ("set", "timeout", 0.1),
                ("set", "write_timeout", 3.0),
                ("set", "exclusive", True),
                ("set", "dtr", False),
                ("set", "rts", False),
                ("open",),
                ("setDTR", False),
                ("setRTS", False),
                ("reset_input_buffer",),
            ],
        )
        self.assertEqual(fake.actions[11], ("write", b"\nFOF_PING\n"))
        self.assertEqual(fake.writes, [b"\nFOF_PING\n"])
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

        self.assertEqual(updates[-1].state, "error")
        self.assertEqual(updates[-1].detail, "wrong_device")
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
        open_index = fake.actions.index(("open",))
        self.assertLess(fake.actions.index(("set", "dtr", False)), open_index)
        self.assertLess(fake.actions.index(("set", "rts", False)), open_index)
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

        self.assertEqual(updates[-1].state, "error")
        self.assertEqual(updates[-1].detail, "read_error")

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
        self.assertEqual(updates[-1].state, "error")


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

        self.assertEqual(updates[-1].state, "error")

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
        self.assertEqual(updates[-1].state, "waiting")
        self.assertEqual(updates[-1].candidates, ((changed.device,),))

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
        self.assertEqual(updates[-1].state, "waiting")

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
    def test_start_racing_a_stop_cannot_leave_a_replacement_worker_live(self) -> None:
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

        transport = BadgeSerialTransport(
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
