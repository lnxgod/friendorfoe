import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_BASE = "2cca5ad8df17ebd8d5f48dc72051441e30df1b8f"
EXPECTED_MANIFEST = {
    "base": EXPECTED_BASE,
    "shared": [
        "constants.h",
        "detection_types.h",
        "detection_policy.c",
        "detection_policy.h",
        "badge_ble_rssi_policy.h",
        "privacy_rf_signatures.c",
        "privacy_rf_signatures.h",
        "psram_alloc.c",
        "psram_alloc.h",
        "rssi_distance.h",
        "time_sync_policy.c",
        "time_sync_policy.h",
        "scanner_uart_line_framer.c",
        "scanner_uart_line_framer.h",
        "ble_investigation_types.h",
        "ble_investigation_protocol.c",
        "ble_investigation_protocol.h",
        "firmware_version_order.c",
        "firmware_version_order.h",
        "firmware_image_contract.c",
        "firmware_image_contract.h",
        "firmware_json_schema.c",
        "firmware_json_schema.h",
        "firmware_operation_token.c",
        "firmware_operation_token.h",
    ],
    "scanner_detection": [
        "bayesian_fusion.c",
        "bayesian_fusion.h",
        "ble_fingerprint.c",
        "ble_fingerprint.h",
        "ble_investigator.c",
        "ble_investigator.h",
        "ble_ja3.c",
        "ble_ja3.h",
        "ble_remote_id.c",
        "ble_remote_id.h",
        "ble_threat_detector.c",
        "ble_threat_detector.h",
        "dji_drone_id_parser.c",
        "dji_drone_id_parser.h",
        "french_dri_parser.c",
        "french_dri_parser.h",
        "glasses_detector.c",
        "glasses_detector.h",
        "open_drone_id_parser.c",
        "open_drone_id_parser.h",
        "wifi_beacon_rid_parser.c",
        "wifi_beacon_rid_parser.h",
        "wifi_oui_database.c",
        "wifi_oui_database.h",
        "wifi_scanner.c",
        "wifi_scanner.h",
        "wifi_ssid_patterns.c",
        "wifi_ssid_patterns.h",
    ],
    "shared_reference": [
        "badge_threat_policy.c",
        "badge_threat_policy.h",
        "uart_protocol.h",
    ],
    "scanner_reference": [
        "comms/uart_ota.c",
        "comms/uart_ota.h",
        "comms/uart_tx.c",
        "comms/uart_tx.h",
        "core/calibration_mode.c",
        "core/calibration_mode.h",
        "core/scanner_rollback.c",
        "core/scanner_rollback.h",
        "core/task_priorities.h",
    ],
}


def _load_module(name: str, path: Path):
    assert path.is_file(), f"missing implementation: {path}"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _auditor():
    return _load_module("backend_source_isolation", ROOT / "tools/check_source_isolation.py")


def _vendor_tool():
    return _load_module("backend_vendor_snapshot", ROOT / "tools/vendor_snapshot.py")


def make_backend_tree(tmp_path: Path) -> Path:
    root = tmp_path / "backend-firmware"
    (root / "shared").mkdir(parents=True)
    (root / "shared/backend_identity.c").write_text("int identity;\n", encoding="utf-8")
    return root


def write_compile_database(root: Path, *, file: Path, include: Path | None = None) -> None:
    arguments = ["cc"]
    if include is not None:
        arguments.extend(["-I", str(include)])
    arguments.extend(["-c", str(file)])
    (root / "compile_commands.json").write_text(
        json.dumps([{"directory": str(root), "file": str(file), "arguments": arguments}]),
        encoding="utf-8",
    )


def test_rejects_protected_include(tmp_path: Path) -> None:
    root = tmp_path / "backend-firmware"
    root.mkdir(parents=True)
    (root / "CMakeLists.txt").write_text(
        'target_sources(app PRIVATE "../esp32/scanner/main/main.c")\n',
        encoding="utf-8",
    )
    assert _auditor().audit_tree(root, allowed_tool_roots=[]) == [
        "CMakeLists.txt: protected or escaping build path ../esp32/scanner/main/main.c"
    ]


def test_rejects_variable_angle_and_bare_protected_build_paths(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    protected = root.parent / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("int protected_source;\n", encoding="utf-8")
    (root / "CMakeLists.txt").write_text(
        "\n".join(
            [
                'target_sources(app PRIVATE "${CMAKE_SOURCE_DIR}/../esp32/scanner/main/main.c")',
                "target_sources(app PRIVATE vendor/shared/detection_policy.c)",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    (root / "shared/angle.c").write_text(
        f"#include <{protected}>\n", encoding="utf-8"
    )

    findings = "\n".join(_auditor().audit_tree(root, allowed_tool_roots=[]))
    assert "${CMAKE_SOURCE_DIR}/../esp32/scanner/main/main.c" in findings
    assert str(protected) in findings
    assert "vendor source compiled vendor/shared/detection_policy.c" in findings


def test_fails_closed_on_unresolved_build_path_expression(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    (root / "CMakeLists.txt").write_text(
        'target_sources(app PRIVATE "$ENV{UNTRUSTED_ROOT}/scanner.c")\n',
        encoding="utf-8",
    )

    assert "unresolved build path" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )


def test_expands_known_local_build_variables_without_escaping(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    (root / "CMakeLists.txt").write_text(
        "\n".join(
            [
                'target_sources(app PRIVATE "${CMAKE_SOURCE_DIR}/shared/backend_identity.c")',
                'target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shared")',
                'target_include_directories(app PRIVATE "${PROJECT_DIR}/shared")',
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    assert _auditor().audit_tree(root, allowed_tool_roots=[]) == []


def test_compile_database_accepts_canonical_absolute_local_path(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    local = (root / "shared/backend_identity.c").resolve()
    write_compile_database(root, file=local, include=root / "shared")
    assert _auditor().audit_tree(root, allowed_tool_roots=[]) == []


def test_compile_database_rejects_absolute_protected_and_vendor_paths(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    repo = root.parent
    protected = repo / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    write_compile_database(root, file=protected)
    assert "protected" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )

    vendor = root / "vendor/shared/constants.c"
    vendor.parent.mkdir(parents=True)
    vendor.write_text("int donor;\n", encoding="utf-8")
    write_compile_database(root, file=vendor)
    assert "vendor source compiled" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )


def test_accepts_absolute_compiler_include_under_explicit_tool_root(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    tool_root = tmp_path.parent / f"{tmp_path.name}-toolchain"
    include = tool_root / "include"
    include.mkdir(parents=True)
    write_compile_database(
        root, file=root / "shared/backend_identity.c", include=include
    )

    assert _auditor().audit_tree(root, allowed_tool_roots=[tool_root]) == []


@pytest.mark.parametrize("variable", ["IDF_PATH", "PLATFORMIO_CORE_DIR"])
@pytest.mark.parametrize("hostile_root", ["repository", "filesystem"])
def test_hostile_environment_tool_roots_cannot_allow_protected_paths(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    variable: str,
    hostile_root: str,
) -> None:
    root = make_backend_tree(tmp_path)
    protected = root.parent / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("int protected_source;\n", encoding="utf-8")
    (root / "CMakeLists.txt").write_text(
        f'target_sources(app PRIVATE "{protected}")\n', encoding="utf-8"
    )
    monkeypatch.delenv("IDF_PATH", raising=False)
    monkeypatch.delenv("PLATFORMIO_CORE_DIR", raising=False)
    monkeypatch.setenv(variable, str(root.parent if hostile_root == "repository" else Path("/")))

    assert "protected" in "\n".join(_auditor().audit_tree(root))


def test_rejects_repository_symlink_even_when_it_resolves_locally(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    linked = root / "linked-shared"
    linked.symlink_to(root / "shared", target_is_directory=True)
    (root / "CMakeLists.txt").write_text(
        'target_include_directories(app PRIVATE "linked-shared")\n',
        encoding="utf-8",
    )

    assert "repository symlink" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )


def test_ignores_provenance_records_and_vendor_evidence(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    (root / "tools").mkdir()
    (root / "tools/vendor_snapshot.py").write_text(
        'SOURCE = "../esp32/shared/constants.h"\n', encoding="utf-8"
    )
    (root / "VENDOR_MANIFEST.json").write_text(
        json.dumps({"shared": ["../esp32/shared/constants.h"]}), encoding="utf-8"
    )
    vendor = root / "vendor/shared"
    vendor.mkdir(parents=True)
    (vendor / "donor.c").write_text(
        '#include "../../../../esp32/shared/constants.h"\n', encoding="utf-8"
    )
    (root / "VENDORED_SHA256.json").write_text(
        json.dumps({"../esp32/shared/constants.h": "00"}), encoding="utf-8"
    )

    assert _auditor().audit_tree(root, allowed_tool_roots=[]) == []


def test_ignores_generated_platformio_dependency_build_files(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    dependency = root / ".pio/libdeps/backend-native/Unity"
    dependency.mkdir(parents=True)
    (dependency / "CMakeLists.txt").write_text(
        'target_include_directories(unity PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")\n',
        encoding="utf-8",
    )

    assert _auditor().audit_tree(root, allowed_tool_roots=[]) == []


def test_still_checks_generated_compile_databases_under_platformio(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    protected = root.parent / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    database_root = root / ".pio/build/backend-native"
    database_root.mkdir(parents=True)
    write_compile_database(database_root, file=protected)

    assert "protected" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )


def test_checks_compile_databases_beneath_vendor(tmp_path: Path) -> None:
    root = make_backend_tree(tmp_path)
    vendor = root / "vendor/shared/detection_policy.c"
    vendor.parent.mkdir(parents=True)
    vendor.write_text("int donor;\n", encoding="utf-8")
    database_root = root / "vendor/generated"
    database_root.mkdir(parents=True)
    write_compile_database(database_root, file=vendor)

    assert "vendor source compiled" in "\n".join(
        _auditor().audit_tree(root, allowed_tool_roots=[])
    )


def test_manifest_is_exactly_pinned_to_the_portable_and_reference_groups() -> None:
    manifest = json.loads((ROOT / "VENDOR_MANIFEST.json").read_text(encoding="utf-8"))
    assert (ROOT / "VENDOR_BASE").read_text(encoding="ascii") == EXPECTED_BASE + "\n"
    assert manifest == EXPECTED_MANIFEST
    declared = [
        entry
        for group in (
            "shared",
            "scanner_detection",
            "shared_reference",
            "scanner_reference",
        )
        for entry in manifest[group]
    ]
    assert len(declared) == len(set(declared))


def test_vendor_tool_rejects_unsafe_manifest_entries(tmp_path: Path) -> None:
    tool = _vendor_tool()
    for unsafe in ("../constants.h", "/tmp/constants.h", "sub/../../constants.h"):
        manifest = {
            "base": EXPECTED_BASE,
            "shared": [unsafe],
            "scanner_detection": [],
            "shared_reference": [],
            "scanner_reference": [],
        }
        manifest_path = tmp_path / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            tool.load_manifest(manifest_path)
        except ValueError as exc:
            assert "unsafe vendor path" in str(exc)
        else:
            raise AssertionError(f"unsafe path accepted: {unsafe}")


def test_vendor_tool_snapshots_manifest_blobs_to_an_explicit_output_only(
    tmp_path: Path,
) -> None:
    tool = _vendor_tool()
    repository = ROOT.parent
    manifest = json.loads((ROOT / "VENDOR_MANIFEST.json").read_text(encoding="utf-8"))
    manifest["shared"] = ["constants.h"]
    manifest["scanner_detection"] = []
    manifest["shared_reference"] = []
    manifest["scanner_reference"] = []
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    output = tmp_path / "snapshot"

    hashes = tool.snapshot(repository, manifest_path, output)

    copied = output / "vendor/shared/constants.h"
    expected = subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "show",
            f"{EXPECTED_BASE}:esp32/shared/constants.h",
        ],
        check=True,
        capture_output=True,
    ).stdout
    assert copied.read_bytes() == expected
    assert hashes == json.loads(
        (output / "VENDORED_SHA256.json").read_text(encoding="utf-8")
    )
    assert list((output / "vendor/shared").iterdir()) == [copied]


def _minimal_manifest(tmp_path: Path) -> Path:
    manifest = {key: list(value) if isinstance(value, list) else value for key, value in EXPECTED_MANIFEST.items()}
    manifest["shared"] = ["constants.h"]
    manifest["scanner_detection"] = []
    manifest["shared_reference"] = []
    manifest["scanner_reference"] = []
    path = tmp_path / "minimal-manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_vendor_tool_rejects_symlinked_destination_component(tmp_path: Path) -> None:
    tool = _vendor_tool()
    output = tmp_path / "snapshot"
    (output / "vendor").mkdir(parents=True)
    unrelated = output / "unrelated"
    unrelated.mkdir()
    (output / "vendor/shared").symlink_to(unrelated, target_is_directory=True)

    with pytest.raises(ValueError, match="symlink"):
        tool.snapshot(ROOT.parent, _minimal_manifest(tmp_path), output)
    assert not (unrelated / "constants.h").exists()


def test_vendor_tool_does_not_follow_provenance_symlink(tmp_path: Path) -> None:
    tool = _vendor_tool()
    output = tmp_path / "snapshot"
    output.mkdir()
    victim = tmp_path / "victim.json"
    victim.write_text("keep me\n", encoding="utf-8")
    (output / "VENDORED_SHA256.json").symlink_to(victim)

    with pytest.raises(ValueError, match="symlink"):
        tool.snapshot(ROOT.parent, _minimal_manifest(tmp_path), output)
    assert victim.read_text(encoding="utf-8") == "keep me\n"
