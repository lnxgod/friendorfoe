import importlib.util
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_BASE = "2cca5ad8df17ebd8d5f48dc72051441e30df1b8f"


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
    tool_root = tmp_path / "toolchain"
    include = tool_root / "include"
    include.mkdir(parents=True)
    write_compile_database(
        root, file=root / "shared/backend_identity.c", include=include
    )

    assert _auditor().audit_tree(root, allowed_tool_roots=[tool_root]) == []


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


def test_manifest_is_exactly_pinned_to_the_portable_and_reference_groups() -> None:
    manifest = json.loads((ROOT / "VENDOR_MANIFEST.json").read_text(encoding="utf-8"))
    assert (ROOT / "VENDOR_BASE").read_text(encoding="ascii") == EXPECTED_BASE + "\n"
    assert manifest["base"] == EXPECTED_BASE
    assert set(manifest) == {
        "base",
        "shared",
        "scanner_detection",
        "shared_reference",
        "scanner_reference",
    }
    assert len(manifest["shared"]) == 25
    assert len(manifest["scanner_detection"]) == 28
    assert len(manifest["shared_reference"]) == 3
    assert len(manifest["scanner_reference"]) == 9


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
