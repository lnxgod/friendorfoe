import struct
import zipfile

from check_android_native_alignment import check_archive


def elf(align, offset=0, machine=183):
    data = bytearray(120)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<H", data, 18, machine)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, offset, 0, 0, 0, 0, align)
    return data


def test_64_bit_packaging_gate_checks_dynamic_features_too(tmp_path):
    archive = tmp_path / "app.aab"
    with zipfile.ZipFile(archive, "w") as bundle:
        bundle.writestr("base/lib/arm64-v8a/libone.so", elf(16384))
        bundle.writestr("feature/lib/x86_64/libtwo.so", elf(4096, machine=62))
        bundle.writestr("feature/lib/arm64-v8a/libthree.so", elf(16384, offset=4096))
    count, errors = check_archive(archive)
    assert count == 3
    assert len(errors) == 2 and any("libtwo" in e for e in errors) and any("libthree" in e for e in errors)


def test_packaging_gate_accepts_aligned_apk_and_rejects_empty_bundle(tmp_path):
    archive = tmp_path / "app.apk"
    with zipfile.ZipFile(archive, "w") as apk:
        apk.writestr("lib/arm64-v8a/libpass.so", elf(65536))
    assert check_archive(archive) == (1, [])
    with zipfile.ZipFile(archive, "w"):
        pass
    assert check_archive(archive)[1]
