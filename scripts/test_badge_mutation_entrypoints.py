#!/usr/bin/env python3
"""Contract and real-entry tests for the badge mutation registry."""

from __future__ import annotations

import ast
import copy
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = REPO_ROOT / "scripts/badge_mutation_entrypoints.json"
INVENTORY_PATH = (
    REPO_ROOT
    / ".superpowers/sdd/2026-07-21-badge-usb-flashing-hardening"
    / "task-10-e0-registry-inventory.md"
)
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import fof_badge_flash as badge_flash


RECORD_FIELDS = (
    "id",
    "source",
    "symbol_or_action",
    "mutation_class",
    "supported_targets",
    "disposition",
    "canonical_implementation",
    "behavior_test",
    "coverage",
    "blocked_by",
)
MUTATION_CLASSES = frozenset({
    "artifact-consume",
    "binary-fetch",
    "binary-stream",
    "browser-flash",
    "control",
    "control/flash",
    "control/reset",
    "factory-flash",
    "flash",
    "flash/reset",
    "identity-ingress",
    "network-fetch",
    "network-ota",
    "persistent-write",
    "physical-reset",
    "process-mutation",
    "publication",
    "reset",
    "rollout",
    "rom-flash",
    "rom-probe",
    "serial-open",
    "staging",
    "topology-probe",
})
TARGETS = frozenset({
    "all",
    "badge",
    "badge-uplink",
    "badge-scanner",
    "factory-board",
    "factory-probe",
    "factory-bundle",
    "scanner-slot",
    "non-badge",
})
DISPOSITIONS = frozenset({
    "bound",
    "bound-wrapper",
    "disabled",
    "non-badge",
    "bound-publication",
    "non-mutating",
    "barrier",
})
PHASES = frozenset({"E0", "A", "B", "C", "D", "E1", "E3", "E4", "E5"})
COVERAGE = frozenset({"red", "green"})
GREEN_IDS = frozenset({
    "badge-upload-scanner-network",
    "badge-relay-scanner-network",
    "badge-flash-uplink-network",
})
PLANNED_IDS = frozenset({
    "badge-factory-load-bundle-bytes",
    "badge-factory-bound-rom-probe",
    "ci-firmware-publication-concurrency",
    "ci-firmware-attest-archive",
    "ci-firmware-protected-publisher",
    "ci-android-attest-assets",
    "ci-android-publication-concurrency",
    "ci-promote-release-by-id",
    "ci-deploy-pages-by-id",
})


def _unwrap_backticks(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value.startswith("`") and value.endswith("`"):
        return value[1:-1]
    return value


def _behavior_test(record_id: str) -> str:
    return (
        "scripts/test_badge_mutation_entrypoints.py::test_"
        + record_id.replace("-", "_")
    )


def parse_inventory_rows(text: str | None = None) -> list[dict]:
    if text is None:
        text = INVENTORY_PATH.read_text(encoding="utf-8")
    records: list[dict] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.startswith("- `"):
            continue
        fields = line.split(" | ")
        if len(fields) != 7:
            raise ValueError(
                f"inventory row {line_number} has {len(fields)} compact fields"
            )
        match = re.fullmatch(r"- `([^`]+)` — `([^`]+)`", fields[0])
        if match is None:
            raise ValueError(f"inventory row {line_number} has invalid prefix")
        record_id, source_action = match.groups()
        if "::" not in source_action:
            raise ValueError(
                f"inventory row {line_number} omits source/action delimiter"
            )
        source, action = source_action.split("::", 1)
        coverage_marker = fields[6].strip()
        if coverage_marker not in {"R", "G"}:
            raise ValueError(
                f"inventory row {line_number} has invalid coverage marker"
            )
        records.append({
            "id": record_id,
            "source": source,
            "symbol_or_action": action,
            "mutation_class": fields[1].strip(),
            "supported_targets": fields[2].strip().split(","),
            "disposition": fields[3].strip(),
            "canonical_implementation": _unwrap_backticks(fields[4]),
            "behavior_test": _behavior_test(record_id),
            "coverage": {"R": "red", "G": "green"}[coverage_marker],
            "blocked_by": fields[5].strip().split(","),
        })
    return records


def load_registry() -> dict:
    return json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))


def _resolve_behavior_test(reference: str) -> unittest.TestCase:
    """Resolve a class-free registry test ID to one unique unittest case."""
    module_path, separator, test_name = reference.partition("::")
    if (
        separator != "::"
        or module_path != "scripts/test_badge_mutation_entrypoints.py"
        or re.fullmatch(r"test_[a-z0-9_]+", test_name) is None
    ):
        raise ValueError(f"invalid behavior test reference: {reference}")

    candidates: list[type[unittest.TestCase]] = []
    for value in globals().values():
        if (
            isinstance(value, type)
            and issubclass(value, unittest.TestCase)
            and callable(getattr(value, test_name, None))
        ):
            candidates.append(value)
    if len(candidates) != 1:
        raise ValueError(
            f"behavior test reference resolves to {len(candidates)} cases: "
            f"{reference}"
        )
    return candidates[0](methodName=test_name)


def validate_registry(registry: dict) -> None:
    if list(registry) != ["schema_version", "e0_status", "records"]:
        raise ValueError("registry top-level fields or order are invalid")
    if type(registry["schema_version"]) is not int or \
            registry["schema_version"] != 1:
        raise ValueError("registry schema_version is invalid")
    if registry["e0_status"] != "red":
        raise ValueError("registry e0_status must remain red")
    records = registry["records"]
    if type(records) is not list or len(records) != 207:
        raise ValueError("registry must contain exactly 207 records")

    seen_ids: set[str] = set()
    seen_actions: set[tuple[str, str]] = set()
    green_ids: set[str] = set()
    for index, record in enumerate(records):
        if type(record) is not dict or list(record) != list(RECORD_FIELDS):
            raise ValueError(f"record {index} fields or field order are invalid")
        record_id = record["id"]
        if (
            type(record_id) is not str
            or re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", record_id) is None
        ):
            raise ValueError(f"record {index} id is invalid")
        if record_id in seen_ids:
            raise ValueError(f"duplicate registry id: {record_id}")
        seen_ids.add(record_id)

        source_action = (record["source"], record["symbol_or_action"])
        if not all(
            type(value) is str and value
            for value in source_action
        ):
            raise ValueError(f"record {record_id} source/action is invalid")
        if source_action in seen_actions:
            raise ValueError(
                f"duplicate registry source/action: {source_action!r}"
            )
        seen_actions.add(source_action)

        if record["mutation_class"] not in MUTATION_CLASSES:
            raise ValueError(f"record {record_id} mutation_class is invalid")
        targets = record["supported_targets"]
        if (
            type(targets) is not list
            or not targets
            or any(type(target) is not str or target not in TARGETS
                   for target in targets)
            or len(targets) != len(set(targets))
        ):
            raise ValueError(f"record {record_id} targets are invalid")
        if record["disposition"] not in DISPOSITIONS:
            raise ValueError(f"record {record_id} disposition is invalid")
        if (
            type(record["canonical_implementation"]) is not str
            or not record["canonical_implementation"].strip()
        ):
            raise ValueError(
                f"record {record_id} canonical implementation is invalid"
            )
        expected_test = _behavior_test(record_id)
        if record["behavior_test"] != expected_test:
            raise ValueError(
                f"record {record_id} behavior-test derivation is invalid"
            )
        if record["coverage"] not in COVERAGE:
            raise ValueError(f"record {record_id} coverage is invalid")
        blocked_by = record["blocked_by"]
        if (
            type(blocked_by) is not list
            or not blocked_by
            or any(type(phase) is not str or phase not in PHASES
                   for phase in blocked_by)
            or len(blocked_by) != len(set(blocked_by))
        ):
            raise ValueError(f"record {record_id} blocked_by is invalid")
        if record["coverage"] == "green":
            try:
                _resolve_behavior_test(record["behavior_test"])
            except ValueError as error:
                raise ValueError(
                    f"green behavior test missing for record {record_id}"
                ) from error
            green_ids.add(record_id)

    if green_ids != GREEN_IDS:
        raise ValueError(
            f"registry green IDs are invalid: {sorted(green_ids)!r}"
        )


def _python_symbols(source: Path) -> set[str]:
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    symbols: set[str] = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            symbols.add(node.name)
        elif isinstance(node, ast.ClassDef):
            symbols.add(node.name)
            for child in node.body:
                if isinstance(
                    child,
                    (ast.FunctionDef, ast.AsyncFunctionDef),
                ):
                    symbols.add(f"{node.name}.{child.name}")
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = (
                node.targets if isinstance(node, ast.Assign) else [node.target]
            )
            for target in targets:
                if isinstance(target, ast.Name):
                    symbols.add(target.id)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                symbols.add(alias.asname or alias.name.split(".", 1)[0])
    return symbols


def _assert_current_action_resolves(test: unittest.TestCase, record: dict) -> None:
    source = REPO_ROOT / record["source"]
    test.assertTrue(source.exists(), f"missing current source for {record['id']}")
    action = record["symbol_or_action"]
    head = action.split(None, 1)[0]
    suffix = source.suffix
    text = source.read_text(encoding="utf-8", errors="replace")

    if suffix == ".py" and re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*",
        head,
    ):
        test.assertIn(
            head,
            _python_symbols(source),
            f"unresolved Python action for {record['id']}",
        )
    elif suffix == ".c":
        if action == "HTTP/auto-check registration":
            for marker in (
                "#ifndef FOF_BADGE_VARIANT",
                "fw_auto_check_init();",
                "Badge firmware network auto-check disabled; ",
            ):
                test.assertIn(
                    marker,
                    text,
                    f"unresolved C registration for {record['id']}",
                )
        elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", head):
            if re.search(rf"\b{re.escape(head)}\s*\(", text) is None:
                if action == "raw scanner OTA-abort boot path":
                    test.assertRegex(
                        text,
                        r"(?i)OTA(?:_| )abort",
                        f"unresolved C branch for {record['id']}",
                    )
                else:
                    test.fail(f"unresolved C action for {record['id']}")
        else:
            test.fail(f"unsupported C action for {record['id']}")
    elif suffix in {".kt", ".kts"} and re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*",
        head,
    ):
        test.assertRegex(
            text,
            rf"\b{re.escape(head.rsplit('.', 1)[-1])}\b",
            f"unresolved Kotlin action for {record['id']}",
        )
    elif suffix == ".ini" and head.startswith("[env:"):
        test.assertIn(head, text, f"unresolved PlatformIO env for {record['id']}")
    elif suffix in {".sh", ".command"}:
        if action.startswith("exec "):
            test.assertIn(
                action.removeprefix("exec "),
                text,
                f"unresolved shell exec for {record['id']}",
            )
        elif action == "main dispatch":
            for marker in ('# Main', 'if [ -n "$1" ]; then',
                           'detect_and_flash "$1"'):
                test.assertIn(
                    marker,
                    text,
                    f"unresolved shell dispatch for {record['id']}",
                )
        elif re.search(
            rf"(?:^|\n){re.escape(head)}\s*\(\)",
            text,
        ) is not None:
            test.assertRegex(
                text,
                rf"(?:^|\n){re.escape(head)}\s*\(\)",
                f"unresolved shell action for {record['id']}",
            )
        elif head != "main":
            test.assertIn(
                head.lower(),
                text.lower(),
                f"unresolved shell action for {record['id']}",
            )
        else:
            test.fail(f"unresolved shell action for {record['id']}")
    elif suffix in {".yml", ".yaml"}:
        if "actions/" in action or "softprops/" in action:
            exact_action = re.search(
                r"(?:actions|softprops)/[A-Za-z0-9_.-]+(?:@[A-Za-z0-9_.-]+)?",
                action,
            )
            test.assertIsNotNone(exact_action)
            test.assertIn(exact_action.group(0), text)
        elif action.startswith("on."):
            event = action.split(".", 2)[1]
            test.assertRegex(text, rf"(?m)^\s*{re.escape(event)}:")
        elif action.startswith("jobs."):
            job = action.split(".", 2)[1]
            test.assertRegex(text, rf"(?m)^\s{{2}}{re.escape(job)}:")
        else:
            step = action
            if action.startswith("Attach firmware to release"):
                test.assertIn("name: Attach firmware to release", text)
                if "tar/copy" in action:
                    test.assertIn("tar czf", text)
                    test.assertRegex(text, r"(?m)^\s+cp ")
                elif "gh release " in action:
                    command = action.split(" gh ", 1)[1]
                    command = command.removesuffix(" --clobber")
                    test.assertIn(command, text)
                    if action.endswith(" --clobber"):
                        test.assertIn("--clobber", text)
                return
            for marker in (" gh ", " actions/"):
                step = step.split(marker, 1)[0]
            test.assertIn(
                f"name: {step}",
                text,
                f"unresolved workflow step for {record['id']}",
            )
    elif suffix == ".html":
        html_actions = {
            "badge Web Serial actions": (
                '<esp-web-install-button manifest="manifest-badge-uplink.json">',
                '<esp-web-install-button manifest="manifest-badge-scanner.json">',
                "navigator.serial.requestPort()",
            ),
        }
        test.assertIn(
            action,
            html_actions,
            f"unsupported HTML action for {record['id']}",
        )
        for marker in html_actions[action]:
            test.assertIn(
                marker,
                text,
                f"unresolved HTML action for {record['id']}",
            )
    elif suffix == ".json" and action == "whole manifest":
        json.loads(text)
    else:
        test.fail(
            f"unsupported current source/action for {record['id']}: "
            f"{record['source']}::{action}"
        )


class MutationRegistryContractTests(unittest.TestCase):
    def test_registry_exists_and_is_valid_json(self) -> None:
        self.assertTrue(
            REGISTRY_PATH.is_file(),
            "authoritative badge mutation registry is missing",
        )
        self.assertIsInstance(load_registry(), dict)

    def test_schema_version_requires_exact_integer_one(self) -> None:
        for invalid_version in (True, 1.0):
            registry = copy.deepcopy(load_registry())
            registry["schema_version"] = invalid_version
            with self.subTest(version=invalid_version), self.assertRaisesRegex(
                ValueError, "schema_version"
            ):
                validate_registry(registry)

    def test_registry_schema_and_exact_markdown_transcription(self) -> None:
        registry = load_registry()
        validate_registry(registry)
        self.assertEqual(registry["records"], parse_inventory_rows())

    def test_registry_has_exactly_three_green_real_entries(self) -> None:
        registry = load_registry()
        self.assertEqual(registry["e0_status"], "red")
        green = {
            record["id"] for record in registry["records"]
            if record["coverage"] == "green"
        }
        self.assertEqual(green, GREEN_IDS)
        self.assertEqual(
            sum(record["coverage"] == "red"
                for record in registry["records"]),
            204,
        )

    def test_green_behavior_references_resolve_and_execute(self) -> None:
        green_records = [
            record for record in load_registry()["records"]
            if record["coverage"] == "green"
        ]
        self.assertEqual(
            {record["id"] for record in green_records},
            GREEN_IDS,
        )
        for record in green_records:
            with self.subTest(record=record["id"]):
                resolved = _resolve_behavior_test(record["behavior_test"])
                self.assertTrue(
                    callable(getattr(resolved, resolved._testMethodName))
                )
                resolved.debug()

    def test_current_and_explicitly_planned_source_policy(self) -> None:
        records = load_registry()["records"]
        planned = {
            record["id"] for record in records
            if record["symbol_or_action"].endswith(" (planned)")
        }
        self.assertEqual(planned, PLANNED_IDS)
        for record in records:
            with self.subTest(record=record["id"]):
                if record["id"] not in PLANNED_IDS:
                    _assert_current_action_resolves(self, record)

    def test_current_html_action_mutation_is_rejected(self) -> None:
        record = copy.deepcopy(next(
            record for record in load_registry()["records"]
            if record["id"] == "web-badge-buttons"
        ))
        # Model a coherent Markdown/JSON action drift: the equality contract
        # alone would accept both sources changing to this same absent action.
        record["symbol_or_action"] = "definitely absent HTML action marker"
        with self.assertRaises(AssertionError):
            _assert_current_action_resolves(self, record)

    def test_current_c_registration_mutation_is_rejected(self) -> None:
        record = copy.deepcopy(next(
            record for record in load_registry()["records"]
            if record["id"] == "uplink-main-http-autocheck"
        ))
        record["symbol_or_action"] = "definitely/absent C action marker"
        with self.assertRaises(AssertionError):
            _assert_current_action_resolves(self, record)

    def test_duplicate_id_mutation_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][1]["id"] = registry["records"][0]["id"]
        with self.assertRaisesRegex(ValueError, "duplicate registry id"):
            validate_registry(registry)

    def test_duplicate_source_action_mutation_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][1]["source"] = registry["records"][0]["source"]
        registry["records"][1]["symbol_or_action"] = (
            registry["records"][0]["symbol_or_action"]
        )
        with self.assertRaisesRegex(ValueError, "duplicate registry source/action"):
            validate_registry(registry)

    def test_missing_or_unknown_record_field_mutation_is_rejected(self) -> None:
        for mutation in ("missing", "unknown"):
            registry = copy.deepcopy(load_registry())
            if mutation == "missing":
                del registry["records"][0]["canonical_implementation"]
            else:
                registry["records"][0]["unknown"] = True
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                ValueError, "fields or field order"
            ):
                validate_registry(registry)

    def test_closed_enum_mutations_are_rejected(self) -> None:
        mutations = (
            ("mutation_class", "not-a-class", "mutation_class"),
            ("disposition", "not-a-disposition", "disposition"),
            ("coverage", "yellow", "coverage"),
        )
        for field, value, message in mutations:
            registry = copy.deepcopy(load_registry())
            registry["records"][0][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(
                ValueError, message
            ):
                validate_registry(registry)

    def test_invalid_target_mutation_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][0]["supported_targets"] = ["not-a-target"]
        with self.assertRaisesRegex(ValueError, "targets"):
            validate_registry(registry)

    def test_invalid_phase_mutation_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][0]["blocked_by"] = ["E0", "E2"]
        with self.assertRaisesRegex(ValueError, "blocked_by"):
            validate_registry(registry)

    def test_markdown_json_drift_mutation_is_detected(self) -> None:
        registry = load_registry()
        inventory = copy.deepcopy(parse_inventory_rows())
        inventory[0]["canonical_implementation"] = "drifted boundary"
        with self.assertRaises(AssertionError):
            self.assertEqual(registry["records"], inventory)

    def test_wrong_behavior_test_derivation_mutation_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][0]["behavior_test"] += "_wrong"
        with self.assertRaisesRegex(ValueError, "behavior-test derivation"):
            validate_registry(registry)

    def test_green_row_without_derived_real_test_is_rejected(self) -> None:
        registry = copy.deepcopy(load_registry())
        registry["records"][0]["coverage"] = "green"
        with self.assertRaisesRegex(ValueError, "green behavior test missing"):
            validate_registry(registry)


class GreenDirectNetworkEntryTests(unittest.TestCase):
    class _ForbiddenArtifact:
        def __init__(self, events: list[str]) -> None:
            self.events = events

        def _trip(self, name: str):
            self.events.append(name)
            raise AssertionError(f"{name} reached")

        def exists(self):
            return self._trip("artifact.exists")

        def stat(self):
            return self._trip("artifact.stat")

        def read_bytes(self):
            return self._trip("artifact.read")

        def open(self, *_args, **_kwargs):
            return self._trip("artifact.open")

        def __fspath__(self):
            return self._trip("artifact.fspath")

    class _PoisonPlatform:
        def __init__(self, events: list[str]) -> None:
            self.events = events
            self.artifact = GreenDirectNetworkEntryTests._ForbiddenArtifact(
                events
            )

        def __getitem__(self, key: str):
            self.events.append(f"parameter.index:{key}")
            values = {
                "scanner_name": "scanner-s3-combo-fof_badge",
                "scanner_bin": self.artifact,
                "uplink_bin": self.artifact,
            }
            return values[key]

    class _PoisonBaseUrl:
        def __init__(self, events: list[str]) -> None:
            self.events = events

        def __format__(self, _format_spec: str) -> str:
            self.events.append("url.format")
            raise AssertionError("url.format reached")

        def __str__(self) -> str:
            self.events.append("url.str")
            raise AssertionError("url.str reached")

    def _assert_direct_entry_disabled(self, invoke) -> None:
        for dry_run in (False, True):
            with self.subTest(dry_run=dry_run):
                events: list[str] = []
                platform = self._PoisonPlatform(events)
                base_url = self._PoisonBaseUrl(events)

                def trip(name: str):
                    def reached(*_args, **_kwargs):
                        events.append(name)
                        raise AssertionError(f"{name} reached")

                    return reached

                with mock.patch.object(
                    badge_flash, "urlencode", side_effect=trip("urlencode")
                ), mock.patch.object(
                    badge_flash, "log", side_effect=trip("log")
                ), mock.patch.object(
                    badge_flash, "http_json", side_effect=trip("http_json")
                ), mock.patch.object(
                    badge_flash, "urlopen", side_effect=trip("urlopen")
                ), mock.patch(
                    "builtins.open", side_effect=trip("builtins.open")
                ):
                    with self.assertRaisesRegex(
                        badge_flash.FlashError,
                        r"USB.*UART.*HTTP/AP/LAN firmware mutation is disabled",
                    ):
                        invoke(platform, base_url, dry_run)
                self.assertEqual(events, [])

    def test_badge_upload_scanner_network(self) -> None:
        self._assert_direct_entry_disabled(
            lambda platform, base_url, dry_run: (
                badge_flash.upload_scanner_network(
                    platform,
                    base_url,
                    "0.64.76-badge-defcon34",
                    dry_run,
                )
            )
        )

    def test_badge_relay_scanner_network(self) -> None:
        self._assert_direct_entry_disabled(
            lambda _platform, base_url, dry_run: (
                badge_flash.relay_scanner_network(
                    base_url,
                    "wifi",
                    dry_run,
                    False,
                    False,
                )
            )
        )

    def test_badge_flash_uplink_network(self) -> None:
        self._assert_direct_entry_disabled(
            lambda platform, base_url, dry_run: (
                badge_flash.flash_uplink_network(
                    platform,
                    base_url,
                    dry_run,
                )
            )
        )


if __name__ == "__main__":
    unittest.main()
