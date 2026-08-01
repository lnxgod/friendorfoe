#!/usr/bin/env python3
"""Pure Phase A tests for the badge flasher's strict host JSON decoder."""

from __future__ import annotations

import json
import sys
import traceback
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash


UINT32_MAX = (1 << 32) - 1
INT64_MAX = (1 << 63) - 1

SCANNER_STAGE_READY_CREDIT = {
    "ok": True,
    "partition": "fw_scanner_s3",
    "size": 8192,
    "crc32": 0xA1B2C3D4,
    "sha256": "a" * 64,
    "target": "scanner-s3-combo-fof_badge",
    "name": "scanner-s3-combo-fof_badge",
    "app_project": "fof_badge_scanner",
    "project": "fof_badge_scanner",
    "hardware_type": "seeed_xiao_esp32s3",
    "hardware": "seeed_xiao_esp32s3",
    "version": "0.64.76-badge-defcon34",
    "slot_mask": 3,
    "flow_control": "credit-v1",
    "phase": "ready",
    "received": 0,
    "total": 8192,
    "credit_bytes": 4096,
}
SCANNER_STAGE_FINAL = {
    **SCANNER_STAGE_READY_CREDIT,
    "phase": "final",
    "received": 8192,
    "credit_bytes": 0,
    "generation": 42,
}
SCANNER_STAGE_FAILURE = {
    "ok": False,
    "error": "scanner staging failed",
}
UPLINK_OTA = {
    "ok": True,
    "phase": "credit",
    "partition": "ota_1",
    "received": 4096,
    "total": 8192,
    "credit_bytes": 4096,
    "retryable": False,
    "reboot_required": False,
    "error": "",
}
RELAY_TERMINAL = {
    "ok": True,
    "phase": "final",
    "slot": "ble",
    "uart": "ble",
    "generation": 42,
    "hardware_id": "e0:72:a1:f9:48:58",
    "size": 8192,
    "bytes": 8192,
    "chunks": 8,
    "stage": "done",
    "done": True,
    "error": "",
}
RELAY_PROGRESS = {
    "uart": "wifi",
    "stage": "chunks",
    "bytes": 4096,
    "size": 8192,
    "percent": 50,
    "chunks": 4,
    "nacks": 0,
    "retries": 1,
    "elapsed_s": 22,
    "error": "",
}
UPDATE_SESSION = "0123456789ABCDEF"
UPDATE_MODE_REBOOTING = {
    "ok": True,
    "phase": "rebooting",
    "session": UPDATE_SESSION,
    "retryable": True,
    "reboot_required": True,
}
UPDATE_MODE_ACTIVE = {
    "ok": True,
    "phase": "active",
    "session": UPDATE_SESSION,
    "retryable": False,
    "reboot_required": False,
}
UPDATE_MODE_FINISHING = {
    "ok": True,
    "phase": "finishing",
    "session": UPDATE_SESSION,
    "retryable": False,
    "reboot_required": True,
}
UPDATE_MODE_ABORTING = {
    "ok": True,
    "phase": "aborting",
    "session": UPDATE_SESSION,
    "retryable": False,
    "reboot_required": True,
}
UPDATE_MODE_WAITING = {
    "ok": False,
    "phase": "waiting_for_owner",
    "session": UPDATE_SESSION,
    "retryable": True,
    "reboot_required": False,
    "error": "firmware_operation_active",
}
UPDATE_MODE_BUSY = {
    "ok": False,
    "phase": "busy",
    "session": UPDATE_SESSION,
    "retryable": True,
    "reboot_required": False,
    "error": "campaign_state_busy",
}

VALID_OBJECTS = (
    ("SCANNER_STAGE_READY_CREDIT", SCANNER_STAGE_READY_CREDIT),
    ("SCANNER_STAGE_FINAL", SCANNER_STAGE_FINAL),
    ("SCANNER_STAGE_FAILURE", SCANNER_STAGE_FAILURE),
    ("UPLINK_OTA", UPLINK_OTA),
    ("RELAY_TERMINAL", RELAY_TERMINAL),
    ("RELAY_PROGRESS", RELAY_PROGRESS),
    ("UPDATE_MODE_REBOOTING", UPDATE_MODE_REBOOTING),
    ("UPDATE_MODE_ACTIVE", UPDATE_MODE_ACTIVE),
    ("UPDATE_MODE_FINISHING", UPDATE_MODE_FINISHING),
    ("UPDATE_MODE_ABORTING", UPDATE_MODE_ABORTING),
    ("UPDATE_MODE_WAITING", UPDATE_MODE_WAITING),
    ("UPDATE_MODE_BUSY", UPDATE_MODE_BUSY),
)


def _decoder() -> Any:
    decoder = getattr(flash, "_strict_json_object_loads", None)
    if decoder is None:
        raise AssertionError("_strict_json_object_loads is not implemented")
    return decoder


def _schema_id(name: str) -> Any:
    schema_ids = getattr(flash, "HostJsonSchemaId", None)
    if schema_ids is None:
        raise AssertionError("HostJsonSchemaId is not implemented")
    try:
        return schema_ids[name]
    except KeyError as exc:
        raise AssertionError(f"missing HostJsonSchemaId.{name}") from exc


def _payload(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class StrictHostJsonDecoderTests(unittest.TestCase):
    def decode(self, payload: str, *schema_names: str) -> dict[str, Any]:
        return _decoder()(
            payload,
            label="test host receipt",
            allowed_schema_ids=tuple(
                _schema_id(name) for name in schema_names
            ),
        )

    def assert_rejected(
        self, payload: str, *schema_names: str
    ) -> flash.SerialTransportError:
        with self.assertRaises(flash.SerialTransportError) as caught:
            self.decode(payload, *schema_names)
        return caught.exception

    def test_accepts_each_exact_named_schema(self) -> None:
        for schema_name, expected in VALID_OBJECTS:
            with self.subTest(schema=schema_name):
                actual = self.decode(_payload(expected), schema_name)
                self.assertEqual(actual, expected)
                self.assertIsNot(actual, expected)

    def test_update_mode_value_validator_accepts_only_exact_phase_tuples(
        self,
    ) -> None:
        valid = (
            ("UPDATE_MODE_REBOOTING", UPDATE_MODE_REBOOTING),
            ("UPDATE_MODE_ACTIVE", UPDATE_MODE_ACTIVE),
            ("UPDATE_MODE_FINISHING", UPDATE_MODE_FINISHING),
            ("UPDATE_MODE_ABORTING", UPDATE_MODE_ABORTING),
            ("UPDATE_MODE_WAITING", UPDATE_MODE_WAITING),
            ("UPDATE_MODE_BUSY", UPDATE_MODE_BUSY),
            (
                "UPDATE_MODE_BUSY",
                {
                    **UPDATE_MODE_BUSY,
                    "retryable": False,
                    "error": "session_conflict",
                },
            ),
        )
        for schema_name, receipt in valid:
            with self.subTest(receipt=receipt):
                decoded = self.decode(_payload(receipt), schema_name)
                self.assertEqual(
                    flash._validate_update_mode_receipt(
                        decoded, session=UPDATE_SESSION
                    ),
                    decoded,
                )

        invalid = (
            {**UPDATE_MODE_REBOOTING, "ok": False},
            {**UPDATE_MODE_REBOOTING, "retryable": False},
            {**UPDATE_MODE_REBOOTING, "reboot_required": False},
            {**UPDATE_MODE_ACTIVE, "reboot_required": True},
            {**UPDATE_MODE_FINISHING, "retryable": True},
            {**UPDATE_MODE_ABORTING, "ok": False},
            {**UPDATE_MODE_WAITING, "error": "campaign_state_busy"},
            {**UPDATE_MODE_BUSY, "error": "unrecognized"},
            {
                **UPDATE_MODE_BUSY,
                "retryable": False,
                "error": "campaign_state_busy",
            },
            {**UPDATE_MODE_BUSY, "error": "session_conflict"},
            {**UPDATE_MODE_ACTIVE, "phase": "unknown"},
            {**UPDATE_MODE_ACTIVE, "session": "0123456789abcdef"},
            {**UPDATE_MODE_ACTIVE, "session": "0000000000000000"},
            {**UPDATE_MODE_ACTIVE, "session": "FEDCBA9876543210"},
        )
        for receipt in invalid:
            schema_name = (
                "UPDATE_MODE_WAITING"
                if "error" in receipt and
                receipt.get("phase") == "waiting_for_owner"
                else "UPDATE_MODE_BUSY"
                if "error" in receipt
                else "UPDATE_MODE_ACTIVE"
                if receipt.get("phase") in ("active", "unknown")
                else "UPDATE_MODE_REBOOTING"
                if receipt.get("phase") == "rebooting"
                else "UPDATE_MODE_FINISHING"
                if receipt.get("phase") == "finishing"
                else "UPDATE_MODE_ABORTING"
            )
            with self.subTest(receipt=receipt):
                with self.assertRaises(flash.FlashError):
                    decoded = self.decode(_payload(receipt), schema_name)
                    flash._validate_update_mode_receipt(
                        decoded, session=UPDATE_SESSION
                    )

    def test_update_mode_schemas_reject_missing_extra_and_wrong_wire_types(
        self,
    ) -> None:
        cases = (
            (
                "missing",
                {key: value for key, value in UPDATE_MODE_REBOOTING.items()
                 if key != "session"},
                "UPDATE_MODE_REBOOTING",
            ),
            (
                "extra",
                {**UPDATE_MODE_REBOOTING, "error": ""},
                "UPDATE_MODE_REBOOTING",
            ),
            (
                "wrong type",
                {**UPDATE_MODE_BUSY, "retryable": 1},
                "UPDATE_MODE_BUSY",
            ),
        )
        for name, receipt, schema_name in cases:
            with self.subTest(name=name):
                self.assert_rejected(_payload(receipt), schema_name)

    def test_accepts_json_whitespace_around_one_object(self) -> None:
        payload = " \t\r\n" + _payload(SCANNER_STAGE_FAILURE) + "\r\n\t "

        self.assertEqual(
            self.decode(payload, "SCANNER_STAGE_FAILURE"),
            SCANNER_STAGE_FAILURE,
        )

    def test_accepts_numeric_domain_boundaries(self) -> None:
        ready = {
            **SCANNER_STAGE_READY_CREDIT,
            "size": UINT32_MAX,
            "crc32": UINT32_MAX,
            "received": UINT32_MAX,
            "total": UINT32_MAX,
            "credit_bytes": UINT32_MAX,
        }
        progress = {
            **RELAY_PROGRESS,
            "bytes": UINT32_MAX,
            "elapsed_s": INT64_MAX,
        }

        self.assertEqual(
            self.decode(_payload(ready), "SCANNER_STAGE_READY_CREDIT"),
            ready,
        )
        self.assertEqual(
            self.decode(_payload(progress), "RELAY_PROGRESS"),
            progress,
        )

    def test_printable_diagnostic_string_may_use_unicode_and_escapes(
        self,
    ) -> None:
        payload = '{"ok":false,"error":"café \\u2603"}'

        self.assertEqual(
            self.decode(payload, "SCANNER_STAGE_FAILURE"),
            {"ok": False, "error": "café ☃"},
        )

    def test_rejects_duplicate_members_in_every_object(self) -> None:
        cases = {
            "top-level duplicate first": (
                '{"ok":false,"ok":true,"error":"first"}'
            ),
            "top-level duplicate last": (
                '{"ok":false,"error":"last","ok":true}'
            ),
            "nested duplicate first": (
                '{"error":{"reason":"a","reason":"b","other":1},'
                '"ok":false}'
            ),
            "nested duplicate last": (
                '{"ok":false,"error":{"other":1,"reason":"a",'
                '"reason":"b"}}'
            ),
        }

        for name, payload in cases.items():
            with self.subTest(name=name):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_escaped_protocol_member_names(self) -> None:
        cases = (
            '{"o\\u006b":false,"error":"failure"}',
            '{"ok":false,"err\\u006fr":"failure"}',
        )

        for payload in cases:
            with self.subTest(payload=payload):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_escapes_in_authorization_token_values(self) -> None:
        cases = (
            '{"ok":true,"phase":"re\\u0061dy","partition":"ota_1",'
            '"received":0,"total":1,"credit_bytes":1,'
            '"retryable":false,"reboot_required":false,"error":""}',
            '{"ok":true,"phase":"ready","partition":"ota\\u005f1",'
            '"received":0,"total":1,"credit_bytes":1,'
            '"retryable":false,"reboot_required":false,"error":""}',
        )

        for payload in cases:
            with self.subTest(payload=payload):
                self.assert_rejected(payload, "UPLINK_OTA")

    def test_rejects_noncanonical_authorization_tokens(self) -> None:
        cases = {
            "empty": "",
            "space": "not ready",
            "non-ASCII": "réady",
        }

        for name, phase in cases.items():
            receipt = {**UPLINK_OTA, "phase": phase}
            with self.subTest(name=name):
                self.assert_rejected(_payload(receipt), "UPLINK_OTA")

    def test_rejects_unknown_and_missing_members(self) -> None:
        cases = {
            "unknown": {**SCANNER_STAGE_FAILURE, "identity": "secret"},
            "missing": {"ok": False},
        }

        for name, receipt in cases.items():
            with self.subTest(name=name):
                self.assert_rejected(
                    _payload(receipt), "SCANNER_STAGE_FAILURE"
                )

    def test_rejects_wrong_exact_wire_types(self) -> None:
        cases = {
            "bool string": {**SCANNER_STAGE_FAILURE, "ok": "false"},
            "diagnostic integer": {**SCANNER_STAGE_FAILURE, "error": 7},
            "uint string": {**RELAY_PROGRESS, "bytes": "4096"},
            "int64 string": {**RELAY_PROGRESS, "elapsed_s": "22"},
        }

        for name, receipt in cases.items():
            schema = (
                "SCANNER_STAGE_FAILURE"
                if "FAILURE" in name.upper() or "diagnostic" in name
                or "bool" in name
                else "RELAY_PROGRESS"
            )
            with self.subTest(name=name):
                self.assert_rejected(_payload(receipt), schema)

    def test_numeric_booleans_are_not_json_booleans(self) -> None:
        cases = (
            {**SCANNER_STAGE_FAILURE, "ok": 0},
            {**UPLINK_OTA, "retryable": 1},
            {**UPLINK_OTA, "reboot_required": 0},
        )

        for receipt in cases:
            schema = (
                "SCANNER_STAGE_FAILURE"
                if set(receipt) == {"ok", "error"}
                else "UPLINK_OTA"
            )
            with self.subTest(schema=schema, receipt=receipt):
                self.assert_rejected(_payload(receipt), schema)

    def test_rejects_invalid_uint32_domains(self) -> None:
        cases = {
            "negative": -1,
            "fractional": 1.5,
            "too large": UINT32_MAX + 1,
            "boolean": True,
        }

        for name, value in cases.items():
            receipt = {**RELAY_PROGRESS, "bytes": value}
            with self.subTest(name=name):
                self.assert_rejected(_payload(receipt), "RELAY_PROGRESS")

    def test_rejects_lexically_signed_uint32_zero(self) -> None:
        payload = _payload(RELAY_PROGRESS).replace(
            '"bytes":4096', '"bytes":-0'
        )

        self.assert_rejected(payload, "RELAY_PROGRESS")

    def test_rejects_invalid_nonnegative_int64_domains(self) -> None:
        cases = {
            "negative": -1,
            "fractional": 1.5,
            "too large": INT64_MAX + 1,
            "boolean": False,
        }

        for name, value in cases.items():
            receipt = {**RELAY_PROGRESS, "elapsed_s": value}
            with self.subTest(name=name):
                self.assert_rejected(_payload(receipt), "RELAY_PROGRESS")

    def test_rejects_nonstandard_json_numeric_constants(self) -> None:
        for constant in ("NaN", "Infinity", "-Infinity"):
            payload = '{"ok":false,"error":' + constant + "}"
            with self.subTest(constant=constant):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_non_object_top_level_values(self) -> None:
        for payload in ("[]", '"string"', "7", "true", "null"):
            with self.subTest(payload=payload):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_raw_nul_and_other_raw_controls(self) -> None:
        cases = (
            '{"ok":false,"error":"raw\x00nul"}',
            '{"ok":false,"error":"raw\x1fcontrol"}',
            '{"ok":false,"error":"raw\x7fdelete"}',
            '\x01{"ok":false,"error":"outside"}',
        )

        for payload in cases:
            with self.subTest(payload=repr(payload)):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_escaped_controls_in_any_string(self) -> None:
        cases = (
            '{"ok":false,"error":"escaped\\nnewline"}',
            '{"ok":false,"error":"escaped\\u0000nul"}',
            '{"ok":false,"error":"escaped\\u001fcontrol"}',
            '{"ok":false,"error":"escaped\\u007fdelete"}',
        )

        for payload in cases:
            with self.subTest(payload=payload):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_rejects_trailing_object_value_and_data(self) -> None:
        valid = _payload(SCANNER_STAGE_FAILURE)
        cases = (
            valid + "{}",
            valid + " true",
            valid + " trailing-data",
        )

        for payload in cases:
            with self.subTest(payload=payload):
                self.assert_rejected(payload, "SCANNER_STAGE_FAILURE")

    def test_allowed_schema_selection_is_exact_not_a_union_or_projection(
        self,
    ) -> None:
        self.assert_rejected(
            _payload(SCANNER_STAGE_READY_CREDIT),
            "SCANNER_STAGE_FINAL",
        )
        self.assert_rejected(
            _payload(SCANNER_STAGE_FINAL),
            "SCANNER_STAGE_READY_CREDIT",
        )
        self.assertEqual(
            self.decode(
                _payload(SCANNER_STAGE_FINAL),
                "SCANNER_STAGE_READY_CREDIT",
                "SCANNER_STAGE_FINAL",
            ),
            SCANNER_STAGE_FINAL,
        )

    def test_rejects_empty_duplicate_and_unknown_schema_selections(
        self,
    ) -> None:
        decoder = _decoder()
        failure = _schema_id("SCANNER_STAGE_FAILURE")
        selections = {
            "empty": (),
            "duplicate": (failure, failure),
            "string ID": ("scanner_stage_failure",),
            "unknown object": (object(),),
        }

        for name, selection in selections.items():
            with self.subTest(name=name), self.assertRaises(
                flash.SerialTransportError
            ):
                decoder(
                    _payload(SCANNER_STAGE_FAILURE),
                    label="test host receipt",
                    allowed_schema_ids=selection,
                )

    def test_rejects_non_tuple_schema_selection(self) -> None:
        decoder = _decoder()
        failure = _schema_id("SCANNER_STAGE_FAILURE")

        with self.assertRaises(flash.SerialTransportError):
            decoder(
                _payload(SCANNER_STAGE_FAILURE),
                label="test host receipt",
                allowed_schema_ids=[failure],
            )

    def test_bounded_decoder_rejects_oversized_payload(self) -> None:
        oversized = (
            " " * (flash.SERIAL_RX_BUFFER_MAX + 1)
            + _payload(SCANNER_STAGE_FAILURE)
        )

        self.assert_rejected(oversized, "SCANNER_STAGE_FAILURE")

    def test_every_failure_is_typed_and_hides_raw_identity(self) -> None:
        secret = "e0:72:a1:f9:48:58"
        payload = (
            '{"ok":false,"error":"failure for '
            + secret
            + '"} trailing-private-data'
        )

        try:
            self.decode(payload, "SCANNER_STAGE_FAILURE")
        except Exception as exc:
            self.assertIs(type(exc), flash.SerialTransportError)
            rendered = "".join(
                traceback.format_exception(type(exc), exc, exc.__traceback__)
            )
            self.assertNotIn(payload, rendered)
            self.assertNotIn(secret, rendered)
            self.assertFalse(exc.terminal_unavailable)
            self.assertIsNone(exc.__cause__)
            self.assertIsNone(exc.__context__)
        else:
            self.fail("malformed identity-bearing payload was accepted")


if __name__ == "__main__":
    unittest.main()
