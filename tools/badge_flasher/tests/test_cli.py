from __future__ import annotations

import contextlib
import io
import inspect
import json
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.badge_flasher import cli
from tools.badge_flasher.models import (
    BatchResult,
    PassedFactoryRecord,
    TopologyAssignment,
    UsbDevice,
)
from tools.badge_flasher.records import ManufacturingLedger
from tools.badge_flasher.verify import VerificationError


VERSION = "0.64.76-badge-defcon34"
ASSIGNMENT = TopologyAssignment(
    "E0:72:A1:F9:47:FC",
    "E0:72:A1:F9:49:84",
    "E0:72:A1:F8:4C:58",
)
UPLINK = UsbDevice(
    ASSIGNMENT.uplink_mac,
    "/dev/cu.uplink",
    "ESP32-S3",
    "v0.2",
    "8MB",
    "8MB",
)
BLE = UsbDevice(
    ASSIGNMENT.ble_leaf_mac,
    "/dev/cu.ble",
    "ESP32-S3",
    "v0.2",
    "8MB",
    "8MB",
)
WIFI = UsbDevice(
    ASSIGNMENT.wifi_leaf_mac,
    "/dev/cu.wifi",
    "ESP32-S3",
    "v0.2",
    "8MB",
    "8MB",
)
DEVICES = {
    UPLINK.mac: UPLINK,
    BLE.mac: BLE,
    WIFI.mac: WIFI,
}


class CliSequenceTests(unittest.TestCase):
    def test_operator_prompt_uses_live_output_inside_factory_capture(
        self,
    ) -> None:
        with (
            mock.patch.object(
                cli,
                "_print_live_user_visible",
            ) as live,
            mock.patch("builtins.input", return_value="y"),
        ):
            answer = cli._run_factory_operation(
                lambda: cli._prompt_operator(
                    "ALREADY PASSED A0:B1:C2:D3:E4:F5? [Y/N] > "
                )
            )

        self.assertEqual(answer, "y")
        live.assert_called_once_with(
            "ALREADY PASSED BADGE? [Y/N] > ",
            end="",
            flush=True,
        )

    def _factory_bundle(self, *, version: str = VERSION):
        return SimpleNamespace(
            version=version,
            bundle_sha256="f" * 64,
            layout=lambda _role: {
                "identity": {"target": "uplink-s3-fof_badge"}
            },
        )

    def _passed_records(
        self,
        root: Path,
        *,
        assignment: TopologyAssignment = ASSIGNMENT,
        version: str = VERSION,
        bundle_sha256: str = "f" * 64,
    ) -> tuple[PassedFactoryRecord, ...]:
        ManufacturingLedger(root).record(BatchResult(
            badge_id="F947FC",
            version=version,
            bundle_sha256=bundle_sha256,
            passed=True,
            phase="complete",
            assignment=assignment,
            devices=(),
            runtime={},
            game_seed="normal",
            receipt="rcpt_00000001",
        ))
        return ManufacturingLedger(root).passed_records()

    def _role_only_boundaries(self):
        backend = mock.Mock()
        backend.list_candidate_ports.return_value = [
            device.port for device in DEVICES.values()
        ]
        backend.scan.return_value = DEVICES
        backend.rebind.return_value = DEVICES
        engine = mock.Mock()
        preseed = mock.Mock(return_value={"hardware_id": UPLINK.mac})
        provision = mock.Mock(return_value=SimpleNamespace())
        runtime = mock.Mock(return_value={})
        return backend, engine, preseed, provision, runtime

    def test_exact_pass_offers_role_only_reassignment(self) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(
                cli,
                "_prompt_operator",
                return_value="n",
            ) as prompt,
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaises(cli._RoleOnlyCancelled),
        ):
            cli.run_one(
                SimpleNamespace(allow_rework=False, yes=False),
                plain=True,
                bundle=self._factory_bundle(),
                game_role="infected",
                passed_records=self._passed_records(Path(temp)),
            )

        self.assertEqual(
            prompt.call_args.args[0],
            "ALREADY PASSED // REASSIGN ROLE ONLY? [Y/N] > ",
        )
        engine.flash_and_verify.assert_not_called()
        preseed.assert_not_called()
        provision.assert_not_called()
        runtime.assert_not_called()

    def test_role_only_reassignment_skips_all_flash_operations(self) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "discover_topology") as discover,
            mock.patch.object(cli, "wait_for_preseed_runtime", preseed),
            mock.patch.object(cli, "provision_game_seed", provision),
            mock.patch.object(cli, "wait_for_runtime", runtime),
            mock.patch.object(cli, "_prompt_operator", return_value="y"),
            mock.patch.object(cli.time, "sleep", return_value=None) as sleep,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = cli.run_one(
                SimpleNamespace(allow_rework=False, yes=False),
                plain=True,
                bundle=self._factory_bundle(),
                game_role="normal",
                passed_records=self._passed_records(Path(temp)),
            )

        discover.assert_not_called()
        engine.flash_and_verify.assert_not_called()
        self.assertEqual(
            [call.args[0].mac for call in engine.handoff_to_application.call_args_list],
            [BLE.mac, WIFI.mac, UPLINK.mac],
        )
        preseed.assert_called_once()
        self.assertEqual(provision.call_args.args[1], "normal")
        self.assertEqual(provision.call_args.kwargs["timeout_s"], 60)
        sleep.assert_called_once_with(3)
        runtime.assert_called_once()
        self.assertEqual(result.phase, "reassign")
        self.assertEqual(result.devices, ())

    def test_role_only_cancel_records_nothing(self) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        stdout = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(
                cli,
                "choose_bundle",
                return_value=self._factory_bundle(),
            ),
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "wait_for_preseed_runtime", preseed),
            mock.patch.object(cli, "provision_game_seed", provision),
            mock.patch.object(cli, "wait_for_runtime", runtime),
            mock.patch("builtins.input", side_effect=["", "n"]),
            contextlib.redirect_stdout(stdout),
        ):
            root = Path(temp)
            self._passed_records(root)
            before = (root / "badge-factory.jsonl").read_text(
                encoding="utf-8"
            )
            result = cli.main([
                "--plain",
                "--once",
                "--offline",
                "--game-role",
                "normal",
                "--records",
                temp,
            ])
            after = (root / "badge-factory.jsonl").read_text(
                encoding="utf-8"
            )

        self.assertEqual(result, 0)
        self.assertEqual(after, before)
        self.assertIn("CANCELLED", stdout.getvalue())
        self.assertNotIn("FAIL //", stdout.getvalue())
        engine.handoff_to_application.assert_not_called()
        provision.assert_not_called()

    def test_role_only_requires_current_complete_pass(self) -> None:
        cases = (
            (
                TopologyAssignment(
                    ASSIGNMENT.uplink_mac,
                    "AA:BB:CC:DD:EE:11",
                    "AA:BB:CC:DD:EE:12",
                ),
                VERSION,
                "f" * 64,
            ),
            (ASSIGNMENT, "0.67.1-badge-defcon34", "f" * 64),
            (ASSIGNMENT, VERSION, "e" * 64),
        )
        for prior_assignment, prior_version, prior_digest in cases:
            with self.subTest(
                assignment=prior_assignment,
                version=prior_version,
            ):
                backend, engine, preseed, provision, runtime = (
                    self._role_only_boundaries()
                )
                with (
                    tempfile.TemporaryDirectory() as temp,
                    mock.patch.object(
                        cli,
                        "DeviceBackend",
                        return_value=backend,
                    ),
                    mock.patch.object(cli, "FlashEngine", return_value=engine),
                    mock.patch.object(cli, "_prompt_operator") as prompt,
                    contextlib.redirect_stdout(io.StringIO()),
                    self.assertRaisesRegex(
                        Exception,
                        "prior PASS",
                    ),
                ):
                    cli.run_one(
                        SimpleNamespace(allow_rework=False, yes=False),
                        plain=True,
                        bundle=self._factory_bundle(),
                        game_role="immune",
                        passed_records=self._passed_records(
                            Path(temp),
                            assignment=prior_assignment,
                            version=prior_version,
                            bundle_sha256=prior_digest,
                        ),
                    )
                prompt.assert_not_called()
                engine.flash_and_verify.assert_not_called()

    def test_noninteractive_passed_badge_fails_without_allow_rework(
        self,
    ) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "_prompt_operator") as prompt,
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaisesRegex(Exception, "--allow-rework"),
        ):
            cli.run_one(
                SimpleNamespace(allow_rework=False, yes=True),
                plain=True,
                bundle=self._factory_bundle(),
                game_role="normal",
                passed_records=self._passed_records(Path(temp)),
            )
        prompt.assert_not_called()
        engine.flash_and_verify.assert_not_called()

    def test_allow_rework_retains_full_flash(self) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        engine.flash_and_verify.return_value = SimpleNamespace()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "usb_jtag_app_reset"),
            mock.patch.object(cli, "rebind_probe_ports", return_value=DEVICES),
            mock.patch.object(
                cli,
                "discover_topology",
                return_value=ASSIGNMENT,
            ) as discover,
            mock.patch.object(cli, "provision_game_seed", provision),
            mock.patch.object(cli, "wait_for_runtime", runtime),
            mock.patch.object(
                cli.time,
                "sleep",
                return_value=None,
            ) as sleep,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = cli.run_one(
                SimpleNamespace(allow_rework=True, yes=True),
                plain=True,
                bundle=self._factory_bundle(),
                game_role="immune",
                passed_records=self._passed_records(Path(temp)),
            )

        discover.assert_called_once()
        self.assertEqual(engine.flash_and_verify.call_count, 6)
        self.assertEqual(provision.call_args.kwargs["timeout_s"], 60)
        self.assertEqual(
            sleep.call_args_list,
            [mock.call(1), mock.call(1), mock.call(3)],
        )
        self.assertEqual(result.phase, "complete")

    def test_role_only_records_distinct_reassign_receipt(self) -> None:
        backend, engine, preseed, provision, runtime = (
            self._role_only_boundaries()
        )
        stdout = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(
                cli,
                "choose_bundle",
                return_value=self._factory_bundle(),
            ),
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "wait_for_preseed_runtime", preseed),
            mock.patch.object(cli, "provision_game_seed", provision),
            mock.patch.object(cli, "wait_for_runtime", runtime),
            mock.patch.object(cli, "generate_receipt", return_value="rcpt_REASSIGN"),
            mock.patch("builtins.input", side_effect=["", "y"]),
            contextlib.redirect_stdout(stdout),
        ):
            root = Path(temp)
            self._passed_records(root)
            self.assertEqual(
                cli.main([
                    "--plain",
                    "--once",
                    "--offline",
                    "--game-role",
                    "infected",
                    "--records",
                    temp,
                ]),
                0,
            )
            rows = [
                json.loads(line)
                for line in (root / "badge-factory.jsonl").read_text(
                    encoding="utf-8"
                ).splitlines()
            ]

        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[-1]["phase"], "reassign")
        self.assertEqual(rows[-1]["devices"], [])
        self.assertEqual(rows[-1]["receipt"], "rcpt_REASSIGN")
        self.assertEqual(rows[-1]["game_seed"], "infected")
        self.assertIn("REASSIGNED", stdout.getvalue())
        self.assertNotIn("write/readback", stdout.getvalue())

    def test_prompt_game_role_maps_every_number_to_canonical_seed(
        self,
    ) -> None:
        for selected, expected in (
            ("1", "normal"),
            ("2", "infected"),
            ("3", "immune"),
        ):
            with self.subTest(selected=selected):
                with (
                    mock.patch("builtins.input", side_effect=[selected, "y"]),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(cli.prompt_game_role(plain=True), expected)

    def test_prompt_game_role_retries_invalid_selection_and_confirmation(
        self,
    ) -> None:
        transcript = io.StringIO()
        with (
            mock.patch(
                "builtins.input",
                side_effect=["", "9", "3", "maybe", "n", "2", "Y"],
            ),
            contextlib.redirect_stdout(transcript),
        ):
            self.assertEqual(cli.prompt_game_role(plain=True), "infected")
        self.assertGreaterEqual(transcript.getvalue().count("SELECT [1-3]"), 2)

    def test_prompt_game_role_q_exits_without_a_default(self) -> None:
        with (
            mock.patch("builtins.input", return_value="q"),
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaises(KeyboardInterrupt),
        ):
            cli.prompt_game_role(plain=True)

    def test_prompt_game_role_renders_plain_words_and_ansi_color(self) -> None:
        plain_transcript = io.StringIO()
        ansi_transcript = io.StringIO()
        with (
            mock.patch("builtins.input", side_effect=["1", "y"]),
            contextlib.redirect_stdout(plain_transcript),
        ):
            cli.prompt_game_role(plain=True)
        with (
            mock.patch("builtins.input", side_effect=["1", "y"]),
            contextlib.redirect_stdout(ansi_transcript),
        ):
            cli.prompt_game_role(plain=False)

        for word in (
            "HUMAN",
            "INFECTED",
            "HEALER",
            "BLACK",
            "GREEN",
            "HOT PINK",
        ):
            self.assertIn(word, plain_transcript.getvalue())
        self.assertIn("\x1b[", ansi_transcript.getvalue())

    def test_receipt_uses_only_eight_cryptographic_crockford_characters(
        self,
    ) -> None:
        for _ in range(32):
            receipt = cli.generate_receipt()
            self.assertRegex(
                receipt,
                re.compile(r"^rcpt_[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{8}$"),
            )

    def test_pass_prints_only_role_and_opaque_receipt_but_ledger_is_private(
        self,
    ) -> None:
        assignment = TopologyAssignment(
            "A0:B1:C2:D3:E4:F5",
            "A0:B1:C2:D3:E4:F6",
            "A0:B1:C2:D3:E4:F7",
        )
        batch = BatchResult(
            badge_id="D3E4F5",
            version="0.64.79-badge-defcon34",
            bundle_sha256="f" * 64,
            passed=True,
            phase="complete",
            assignment=assignment,
            devices=(),
            runtime={
                "game_seed": "infected",
                "game_state": "infected",
                "game_active": False,
                "game_shield": 0,
            },
            game_seed="infected",
            receipt="rcpt_K7M2Q9W4",
        )
        bundle = SimpleNamespace(
            version=batch.version,
            bundle_sha256=batch.bundle_sha256,
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "run_one", return_value=batch),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = cli.main([
                "--plain",
                "--once",
                "--yes",
                "--offline",
                "--game-role",
                "infected",
                "--records",
                temp,
            ])
            private_row = json.loads(
                (Path(temp) / "badge-factory.jsonl").read_text(
                    encoding="utf-8"
                )
            )

        transcript = stdout.getvalue() + stderr.getvalue()
        self.assertEqual(result, 0)
        self.assertIn(
            "PASS // GAME ROLE infected // RECEIPT rcpt_K7M2Q9W4",
            transcript,
        )
        self.assertNotIn(batch.badge_id, transcript)
        self.assertNotIn(batch.bundle_sha256, transcript)
        self.assertNotIn(assignment.uplink_mac, transcript)
        self.assertEqual(
            private_row["assignment"]["uplink_mac"],
            assignment.uplink_mac,
        )
        self.assertEqual(private_row["receipt"], batch.receipt)

    def test_seed_failure_has_no_pass_or_receipt_and_records_selected_role(
        self,
    ) -> None:
        bundle = SimpleNamespace(
            version="0.64.79-badge-defcon34",
            bundle_sha256="e" * 64,
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(
                cli,
                "run_one",
                side_effect=VerificationError("seed rejected"),
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = cli.main([
                "--plain",
                "--once",
                "--yes",
                "--offline",
                "--game-role",
                "immune",
                "--records",
                temp,
            ])
            failure = json.loads(
                (Path(temp) / "badge-factory.jsonl").read_text(
                    encoding="utf-8"
                )
            )

        transcript = stdout.getvalue() + stderr.getvalue()
        self.assertEqual(result, 1)
        self.assertNotIn("PASS //", transcript)
        self.assertNotIn("RECEIPT", transcript)
        self.assertEqual(failure["game_seed"], "immune")
        self.assertIsNone(failure["receipt"])

    def test_active_operation_ctrl_c_records_rework_failure(self) -> None:
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="b" * 64,
        )
        stdout = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "run_one", side_effect=KeyboardInterrupt),
            contextlib.redirect_stdout(stdout),
        ):
            self.assertEqual(
                cli.main([
                    "--plain",
                    "--once",
                    "--yes",
                    "--offline",
                    "--game-role",
                    "infected",
                    "--records",
                    temp,
                ]),
                130,
            )
            jsonl = Path(temp) / "badge-factory.jsonl"
            self.assertTrue(jsonl.exists())
            rows = [
                json.loads(line)
                for line in jsonl.read_text(encoding="utf-8").splitlines()
            ]

        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]["passed"])
        self.assertEqual(rows[0]["game_seed"], "infected")
        self.assertEqual(rows[0]["version"], bundle.version)
        self.assertEqual(rows[0]["bundle_sha256"], bundle.bundle_sha256)
        self.assertNotIn("PASS //", stdout.getvalue())
        self.assertIn("rework bin", stdout.getvalue())
        self.assertNotIn("no PASS record written", stdout.getvalue())

    def test_pass_output_ctrl_c_keeps_the_durable_pass(self) -> None:
        assignment = TopologyAssignment(
            "A0:B1:C2:D3:E4:01",
            "A0:B1:C2:D3:E4:02",
            "A0:B1:C2:D3:E4:03",
        )
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="b" * 64,
        )
        passed = BatchResult(
            badge_id="D3E401",
            version=bundle.version,
            bundle_sha256=bundle.bundle_sha256,
            passed=True,
            phase="complete",
            assignment=assignment,
            devices=(),
            runtime={},
            game_seed="immune",
            receipt="rcpt_00000001",
        )
        stdout = io.StringIO()
        original_print = cli.print_user_visible

        def interrupt_pass_output(value: object = "", **kwargs: object) -> None:
            if "PASS //" in str(value):
                raise KeyboardInterrupt
            original_print(value, **kwargs)

        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "run_one", return_value=passed),
            mock.patch.object(cli, "print_user_visible", interrupt_pass_output),
            contextlib.redirect_stdout(stdout),
        ):
            self.assertEqual(
                cli.main([
                    "--plain",
                    "--once",
                    "--yes",
                    "--offline",
                    "--game-role",
                    "immune",
                    "--records",
                    temp,
                ]),
                130,
            )
            rows = [
                json.loads(line)
                for line in (Path(temp) / "badge-factory.jsonl").read_text(
                    encoding="utf-8"
                ).splitlines()
            ]

        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]["passed"])
        self.assertIn("PASS already recorded", stdout.getvalue())
        self.assertNotIn("FAIL //", stdout.getvalue())

    def test_removal_prompt_cancellation_keeps_the_recorded_pass(self) -> None:
        assignment = TopologyAssignment(
            "A0:B1:C2:D3:E4:01",
            "A0:B1:C2:D3:E4:02",
            "A0:B1:C2:D3:E4:03",
        )
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="b" * 64,
        )
        passed = BatchResult(
            badge_id="D3E401",
            version=bundle.version,
            bundle_sha256=bundle.bundle_sha256,
            passed=True,
            phase="complete",
            assignment=assignment,
            devices=(),
            runtime={},
            game_seed="immune",
            receipt="rcpt_00000001",
        )
        for interruption in (EOFError, KeyboardInterrupt):
            with self.subTest(interruption=interruption.__name__):
                stdout = io.StringIO()
                with (
                    tempfile.TemporaryDirectory() as temp,
                    mock.patch.object(cli, "choose_bundle", return_value=bundle),
                    mock.patch.object(cli, "run_one", return_value=passed),
                    mock.patch("builtins.input", side_effect=["", interruption]),
                    contextlib.redirect_stdout(stdout),
                ):
                    self.assertEqual(
                        cli.main([
                            "--plain",
                            "--offline",
                            "--game-role",
                            "immune",
                            "--records",
                            temp,
                        ]),
                        130,
                    )
                    rows = [
                        json.loads(line)
                        for line in (
                            Path(temp) / "badge-factory.jsonl"
                        ).read_text(encoding="utf-8").splitlines()
                    ]

                self.assertEqual(len(rows), 1)
                self.assertTrue(rows[0]["passed"])
                self.assertIn("PASS already recorded", stdout.getvalue())
                self.assertNotIn("no PASS record written", stdout.getvalue())

    def test_connection_prompt_cancellation_writes_no_factory_record(self) -> None:
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="b" * 64,
        )
        for interruption in (EOFError, KeyboardInterrupt):
            with self.subTest(interruption=interruption.__name__):
                stdout = io.StringIO()
                with (
                    tempfile.TemporaryDirectory() as temp,
                    mock.patch.object(cli, "choose_bundle", return_value=bundle),
                    mock.patch.object(cli, "run_one") as run_one,
                    mock.patch("builtins.input", side_effect=interruption),
                    contextlib.redirect_stdout(stdout),
                ):
                    self.assertEqual(
                        cli.main([
                            "--plain",
                            "--offline",
                            "--game-role",
                            "normal",
                            "--records",
                            temp,
                        ]),
                        130,
                    )
                    self.assertFalse(
                        (Path(temp) / "badge-factory.jsonl").exists()
                    )

                run_one.assert_not_called()
                self.assertIn("no PASS record written", stdout.getvalue())
                self.assertNotIn("PASS //", stdout.getvalue())
                self.assertNotIn("FAIL //", stdout.getvalue())

    def test_role_menu_cancellation_writes_no_factory_record(self) -> None:
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="b" * 64,
        )
        for interruption in ("q", EOFError, KeyboardInterrupt):
            with self.subTest(interruption=interruption):
                stdout = io.StringIO()
                with (
                    tempfile.TemporaryDirectory() as temp,
                    mock.patch.object(cli, "choose_bundle", return_value=bundle),
                    mock.patch.object(cli, "run_one") as run_one,
                    mock.patch("builtins.input", side_effect=interruption),
                    contextlib.redirect_stdout(stdout),
                ):
                    self.assertEqual(
                        cli.main([
                            "--plain",
                            "--offline",
                            "--records",
                            temp,
                        ]),
                        130,
                    )
                    self.assertFalse(
                        (Path(temp) / "badge-factory.jsonl").exists()
                    )

                run_one.assert_not_called()
                self.assertIn("no PASS record written", stdout.getvalue())
                self.assertNotIn("PASS //", stdout.getvalue())
                self.assertNotIn("FAIL //", stdout.getvalue())

    def test_parser_has_no_silent_role_default(self) -> None:
        self.assertIsNone(cli.parser().parse_args([]).game_role)
        for role in ("normal", "infected", "immune"):
            with self.subTest(role=role):
                self.assertEqual(
                    cli.parser().parse_args(
                        ["--game-role", role]
                    ).game_role,
                    role,
                )
        with self.assertRaisesRegex(ValueError, "invalid choice"):
            cli.parser().parse_args(["--game-role", "zombie"])

    def test_yes_requires_once_and_explicit_role_before_bundle_selection(
        self,
    ) -> None:
        for argv in (
            ["--yes"],
            ["--yes", "--once"],
            ["--yes", "--game-role", "normal"],
        ):
            with self.subTest(argv=argv):
                with (
                    mock.patch.object(cli, "choose_bundle") as choose_bundle,
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(cli.main(argv), 2)
                choose_bundle.assert_not_called()

    def test_two_interactive_badges_use_two_independent_roles(self) -> None:
        assignments = (
            TopologyAssignment(
                "A0:B1:C2:D3:E4:01",
                "A0:B1:C2:D3:E4:02",
                "A0:B1:C2:D3:E4:03",
            ),
            TopologyAssignment(
                "A0:B1:C2:D3:E4:11",
                "A0:B1:C2:D3:E4:12",
                "A0:B1:C2:D3:E4:13",
            ),
        )
        roles: list[str] = []

        def fake_run(_args, _plain, bundle, *, game_role, **_kwargs):
            roles.append(game_role)
            index = len(roles) - 1
            return BatchResult(
                badge_id=f"badge-{index}",
                version=bundle.version,
                bundle_sha256=bundle.bundle_sha256,
                passed=True,
                phase="complete",
                assignment=assignments[index],
                devices=(),
                runtime={},
                game_seed=game_role,
                receipt=f"rcpt_0000000{index}",
            )

        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(
                cli,
                "choose_bundle",
                return_value=SimpleNamespace(
                    version="0.67.2-badge-defcon34",
                    bundle_sha256="f" * 64,
                ),
            ),
            mock.patch.object(
                cli,
                "prompt_game_role",
                side_effect=["normal", "immune", KeyboardInterrupt],
            ),
            mock.patch.object(cli, "run_one", side_effect=fake_run),
            mock.patch("builtins.input", return_value=""),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                cli.main(["--plain", "--offline", "--records", temp]),
                130,
            )
        self.assertEqual(roles, ["normal", "immune"])

    def test_interactive_failure_records_the_current_role(self) -> None:
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="f" * 64,
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "prompt_game_role", return_value="immune"),
            mock.patch.object(
                cli,
                "run_one",
                side_effect=VerificationError("seed rejected"),
            ),
            mock.patch("builtins.input", return_value=""),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                cli.main(["--plain", "--once", "--offline", "--records", temp]),
                1,
            )
            failure = json.loads(
                (Path(temp) / "badge-factory.jsonl").read_text(encoding="utf-8")
            )
        self.assertEqual(failure["game_seed"], "immune")

    def test_explicit_game_role_skips_role_menu(self) -> None:
        assignment = TopologyAssignment(
            "A0:B1:C2:D3:E4:01",
            "A0:B1:C2:D3:E4:02",
            "A0:B1:C2:D3:E4:03",
        )
        bundle = SimpleNamespace(
            version="0.67.2-badge-defcon34",
            bundle_sha256="f" * 64,
        )
        result = BatchResult(
            badge_id="D3E401",
            version=bundle.version,
            bundle_sha256=bundle.bundle_sha256,
            passed=True,
            phase="complete",
            assignment=assignment,
            devices=(),
            runtime={},
            game_seed="infected",
            receipt="rcpt_00000001",
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "prompt_game_role") as prompt_game_role,
            mock.patch.object(cli, "run_one", return_value=result),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                cli.main([
                    "--plain",
                    "--once",
                    "--yes",
                    "--offline",
                    "--game-role",
                    "infected",
                    "--records",
                    temp,
                ]),
                0,
            )
        prompt_game_role.assert_not_called()

    def test_selected_game_role_is_shown_before_operator_confirmation(
        self,
    ) -> None:
        class PromptReached(RuntimeError):
            pass

        transcript = io.StringIO()

        def stop_at_prompt(_prompt: str = "") -> str:
            self.assertIn("GAME ROLE infected", transcript.getvalue())
            raise PromptReached

        bundle = mock.Mock(
            version="0.64.79-badge-defcon34",
            bundle_sha256="f" * 64,
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch("builtins.input", side_effect=stop_at_prompt),
            contextlib.redirect_stdout(transcript),
            self.assertRaises(PromptReached),
        ):
            cli.main([
                "--plain",
                "--offline",
                "--game-role",
                "infected",
                "--records",
                temp,
            ])

    def test_main_selects_bundle_once_outside_repeat_loop(self) -> None:
        source = inspect.getsource(cli.main)
        selection = source.index("lambda: choose_bundle(args, plain)")
        self.assertLess(selection, source.index("while True"))
        self.assertEqual(
            source.count("lambda: choose_bundle(args, plain)"), 1
        )
        self.assertIn("lambda: run_one(", source)

    def test_repeat_loop_tracks_previous_badge_macs(self) -> None:
        source = inspect.getsource(cli.main)
        self.assertIn("last_badge_macs", source)
        run_source = inspect.getsource(cli.run_one)
        self.assertIn("previous BADGE is still connected", run_source)

    def test_explicit_bundle_is_never_superseded_by_github(self) -> None:
        source = inspect.getsource(cli.choose_bundle)
        override = source.index("if args.bundle:")
        github = source.index("fetch_github_bundles")
        self.assertLess(override, github)

    def test_factory_handoff_boots_scanners_before_uplink(self) -> None:
        calls: list[str] = []
        cli._handoff_factory_graph(
            SimpleNamespace(
                handoff_to_application=lambda device: calls.append(device.mac)
            ),
            DEVICES,
            ASSIGNMENT,
            plain=True,
        )
        self.assertEqual(
            calls,
            [
                ASSIGNMENT.ble_leaf_mac,
                ASSIGNMENT.wifi_leaf_mac,
                ASSIGNMENT.uplink_mac,
            ],
        )

    def _exercise_run_one_seed_retry(
        self,
        seed_side_effect: list[object],
    ) -> tuple[mock.Mock, mock.Mock, mock.Mock, mock.Mock]:
        backend = mock.Mock()
        backend.list_candidate_ports.return_value = [
            device.port for device in DEVICES.values()
        ]
        backend.scan.return_value = DEVICES
        backend.rebind.return_value = DEVICES
        engine = mock.Mock()
        engine.flash_and_verify.return_value = SimpleNamespace()
        bundle = SimpleNamespace(
            version=VERSION,
            bundle_sha256="f" * 64,
            layout=lambda _role: {
                "identity": {"target": "uplink-s3-fof_badge"}
            },
        )
        provision = mock.Mock(side_effect=seed_side_effect)
        runtime = mock.Mock(return_value={})

        with (
            mock.patch.object(cli, "DeviceBackend", return_value=backend),
            mock.patch.object(cli, "FlashEngine", return_value=engine),
            mock.patch.object(cli, "usb_jtag_app_reset"),
            mock.patch.object(
                cli,
                "rebind_probe_ports",
                return_value=DEVICES,
            ),
            mock.patch.object(
                cli,
                "discover_topology",
                return_value=ASSIGNMENT,
            ),
            mock.patch.object(cli, "provision_game_seed", provision),
            mock.patch.object(cli, "wait_for_runtime", runtime),
            mock.patch.object(cli.time, "sleep", return_value=None),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            try:
                cli.run_one(
                    SimpleNamespace(allow_rework=False),
                    plain=True,
                    bundle=bundle,
                    game_role="immune",
                )
            except VerificationError:
                if len(seed_side_effect) < 2:
                    raise

        return backend, engine, provision, runtime

    def test_seed_timeout_rebinds_and_handoffs_once_without_reflash(
        self,
    ) -> None:
        backend, engine, provision, runtime = (
            self._exercise_run_one_seed_retry([
                VerificationError(
                    "game seed provisioning timed out: uplink silent"
                ),
                SimpleNamespace(),
            ])
        )

        self.assertEqual(provision.call_count, 2)
        self.assertEqual(backend.rebind.call_count, 6)
        self.assertEqual(engine.handoff_to_application.call_count, 6)
        self.assertEqual(engine.flash_and_verify.call_count, 6)
        runtime.assert_called_once()

    def test_seed_firmware_rejection_skips_rom_handoff_recovery(self) -> None:
        backend, engine, provision, runtime = (
            self._exercise_run_one_seed_retry([
                VerificationError(
                    "uplink pre-seed identity rejected by firmware: "
                    "FOF_ERROR:unknown command"
                ),
                AssertionError(
                    "firmware rejection must not retry seed provisioning"
                ),
            ])
        )

        self.assertEqual(provision.call_count, 1)
        self.assertEqual(backend.rebind.call_count, 5)
        self.assertEqual(engine.handoff_to_application.call_count, 3)
        self.assertEqual(engine.flash_and_verify.call_count, 6)
        runtime.assert_not_called()

    def test_second_seed_timeout_stops_without_runtime_gate(self) -> None:
        backend, engine, provision, runtime = (
            self._exercise_run_one_seed_retry([
                VerificationError(
                    "game seed provisioning timed out: uplink silent"
                ),
                VerificationError(
                    "game seed provisioning timed out: uplink still silent"
                ),
            ])
        )

        self.assertEqual(provision.call_count, 2)
        self.assertEqual(backend.rebind.call_count, 6)
        self.assertEqual(engine.handoff_to_application.call_count, 6)
        self.assertEqual(engine.flash_and_verify.call_count, 6)
        runtime.assert_not_called()


if __name__ == "__main__":
    unittest.main()
