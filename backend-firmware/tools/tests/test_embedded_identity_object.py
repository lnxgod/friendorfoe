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


def _compile_and_dump(tmp_path: Path, *defines: str) -> bytes:
    cc, objcopy, compiler_flags = _compiler_and_objcopy()
    stem = "_".join(define.lower() for define in defines)
    obj = tmp_path / f"{stem}.o"
    section = tmp_path / f"{stem}.bin"
    compile_result = subprocess.run(
        [
            cc,
            *compiler_flags,
            "-std=c11",
            f"-I{SHARED}",
            *[f"-D{define}" for define in defines],
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
    ("image_define", "profile_define", "kind", "target", "project", "hardware_identity", "expected_crc"),
    [
        (
            "FOF_BACKEND_UPLINK",
            "FOF_BACKEND_PROFILE_BADGE_LITE=1",
            0,
            "uplink-s3-backend",
            "fof_backend_uplink",
            "seeed_xiao_esp32s3",
            0xB42AE8FC,
        ),
        (
            "FOF_BACKEND_SCANNER",
            "FOF_BACKEND_PROFILE_BADGE_LITE=1",
            1,
            "scanner-s3-combo-backend",
            "fof_backend_scanner",
            "seeed_xiao_esp32s3",
            0xD972A7E7,
        ),
        (
            "FOF_BACKEND_UPLINK",
            "FOF_BACKEND_PROFILE_S3_FULLSIZE=1",
            0,
            "uplink-s3-fullsize-backend",
            "fof_backend_uplink_fullsize",
            "esp32s3_n16r8_fullsize",
            0xF03A379D,
        ),
        (
            "FOF_BACKEND_SCANNER",
            "FOF_BACKEND_PROFILE_S3_FULLSIZE=1",
            1,
            "scanner-s3-combo-fullsize-backend",
            "fof_backend_scanner_fullsize",
            "esp32s3_n16r8_fullsize",
            0x86C70497,
        ),
    ],
)
def test_embedded_identity_object_contains_one_exact_static_record(
    tmp_path: Path,
    image_define: str,
    profile_define: str,
    kind: int,
    target: str,
    project: str,
    hardware_identity: str,
    expected_crc: int,
) -> None:
    raw = _compile_and_dump(tmp_path, image_define, profile_define)

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
    _decode_c_string(hardware, hardware_identity)
    _decode_c_string(version, "0.2.0-backend")
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
        "-DFOF_BACKEND_PROFILE_BADGE_LITE=1",
        *[f"-D{define}" for define in defines],
        "-c",
        str(SOURCE),
        "-o",
        str(tmp_path / "ambiguous.o"),
    ]
    result = subprocess.run(command, capture_output=True, text=True)

    assert result.returncode != 0
    assert "exactly one backend image" in result.stderr


@pytest.mark.parametrize(
    "profile_defines",
    [[], ["FOF_BACKEND_PROFILE_BADGE_LITE=1", "FOF_BACKEND_PROFILE_S3_FULLSIZE=1"]],
)
def test_embedded_identity_source_rejects_missing_or_ambiguous_hardware_profile(
    tmp_path: Path, profile_defines: list[str]
) -> None:
    """Catches a backend build that silently defaults or mixes board identities."""
    cc, _objcopy_path, compiler_flags = _compiler_and_objcopy()
    command = [
        cc,
        *compiler_flags,
        "-std=c11",
        f"-I{SHARED}",
        "-DFOF_BACKEND_UPLINK",
        *[f"-D{define}" for define in profile_defines],
        "-c",
        str(SOURCE),
        "-o",
        str(tmp_path / "profile.o"),
    ]
    result = subprocess.run(command, capture_output=True, text=True)

    assert result.returncode != 0
    assert "select exactly one backend hardware profile" in result.stderr
