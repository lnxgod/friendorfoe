import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

import pytest


ROOT = Path(__file__).resolve().parents[2]
SHARED = ROOT / "shared"
SOURCE = SHARED / "backend_embedded_identity.c"
RECORD = struct.Struct("<IHH40s40s40s32sI")


def _tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    raise AssertionError(f"required host tool not found: {name}")


def _compiler_and_objcopy() -> tuple[str, str, list[str]]:
    configured = os.environ.get("OBJCOPY")
    if configured:
        return _tool("cc"), configured, []
    for name in ("objcopy", "llvm-objcopy", "gobjcopy"):
        found = shutil.which(name)
        if found:
            return _tool("cc"), found, []
    packages = Path.home() / ".platformio" / "packages"
    objcopy_candidates = sorted(packages.glob("**/xtensa-esp32s3-elf-objcopy"))
    for objcopy in objcopy_candidates:
        compiler = objcopy.with_name("xtensa-esp32s3-elf-gcc")
        if compiler.is_file():
            return str(compiler), str(objcopy), []
    raise AssertionError("a compatible C compiler and objcopy were not discoverable")


def _compile_and_dump(tmp_path: Path, define: str) -> bytes:
    cc, objcopy, compiler_flags = _compiler_and_objcopy()
    obj = tmp_path / f"{define.lower()}.o"
    section = tmp_path / f"{define.lower()}.bin"
    compile_result = subprocess.run(
        [
            cc,
            *compiler_flags,
            "-std=c11",
            f"-I{SHARED}",
            f"-D{define}",
            "-c",
            str(SOURCE),
            "-o",
            str(obj),
        ],
        capture_output=True,
        text=True,
    )
    assert compile_result.returncode == 0, compile_result.stderr
    dump_result = subprocess.run(
        [
            objcopy,
            "--dump-section",
            f".fof_backend_identity={section}",
            str(obj),
        ],
        capture_output=True,
        text=True,
    )
    assert dump_result.returncode == 0, dump_result.stderr
    return section.read_bytes()


def _decode_c_string(field: bytes, expected: str) -> None:
    encoded = expected.encode("ascii")
    assert field[: len(encoded)] == encoded
    assert field[len(encoded) :] == bytes(len(field) - len(encoded))


@pytest.mark.parametrize(
    ("define", "kind", "target", "project", "expected_crc"),
    [
        (
            "FOF_BACKEND_UPLINK",
            0,
            "uplink-s3-backend",
            "fof_backend_uplink",
            0xF08BCDE4,
        ),
        (
            "FOF_BACKEND_SCANNER",
            1,
            "scanner-s3-combo-backend",
            "fof_backend_scanner",
            0x9DD382FF,
        ),
    ],
)
def test_embedded_identity_object_contains_one_exact_static_record(
    tmp_path: Path,
    define: str,
    kind: int,
    target: str,
    project: str,
    expected_crc: int,
) -> None:
    raw = _compile_and_dump(tmp_path, define)

    assert zlib.crc32(b"123456789") == 0xCBF43926
    assert len(raw) == 164
    magic, schema, actual_kind, target_bytes, project_bytes, hardware, version, crc = (
        RECORD.unpack(raw)
    )
    assert magic == 0x42464F46
    assert schema == 1
    assert actual_kind == kind
    _decode_c_string(target_bytes, target)
    _decode_c_string(project_bytes, project)
    _decode_c_string(hardware, "seeed_xiao_esp32s3")
    _decode_c_string(version, "0.1.0-backend")
    assert crc == expected_crc
    assert zlib.crc32(raw[:160]) & 0xFFFFFFFF == expected_crc


@pytest.mark.parametrize(
    "defines",
    [[], ["FOF_BACKEND_UPLINK", "FOF_BACKEND_SCANNER"]],
)
def test_embedded_identity_source_rejects_ambiguous_image_selection(
    tmp_path: Path, defines: list[str]
) -> None:
    cc, _objcopy_path, compiler_flags = _compiler_and_objcopy()
    command = [
        cc,
        *compiler_flags,
        "-std=c11",
        f"-I{SHARED}",
        *[f"-D{define}" for define in defines],
        "-c",
        str(SOURCE),
        "-o",
        str(tmp_path / "ambiguous.o"),
    ]
    result = subprocess.run(command, capture_output=True, text=True)

    assert result.returncode != 0
    assert "exactly one backend image" in result.stderr
