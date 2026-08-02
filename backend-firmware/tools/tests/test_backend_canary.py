from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess

import pytest

from tools.backend_canary import (
    BACKEND_PARTITIONS,
    BOARD_ROLES,
    FLASH_SIZE,
    LEGACY_PARTITIONS,
    PINNED_UPDATER_ADMISSION_SHA256,
    ApprovalChallenge,
    BackupRecord,
    BoardIdentity,
    CanaryApprovalError,
    CanaryBackupError,
    CanaryInventoryError,
    CanaryOrderError,
    CanaryReleaseError,
    CanarySecurityError,
    CanaryState,
    OtaEvidence,
    ToolchainReceipt,
    build_backup_commands,
    build_initial_flash_command,
    build_inventory_commands,
    build_ota_apply_line,
    build_restore_command,
    canonical_partition_sha256,
    decode_partition_table,
    execute_backup,
    execute_initial_flash,
    issue_initial_flash_challenge,
    parse_evidence_line,
    parse_live_inventory,
    parse_ota_evidence,
    redact_secrets,
    resolve_toolchain,
    validate_flash_ranges,
    validate_final_health_set,
    validate_provisional_identity,
    verify_original_uplink_quiescence,
    verify_release_artifact,
)


def private_file(path: Path, payload: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.write_bytes(payload)
    path.chmod(0o600)
    return path


def toolchain(seed: str = "a") -> ToolchainReceipt:
    return ToolchainReceipt(
        pio_path=f"/opt/platformio-{seed}/pio",
        platformio_version="6.1.18",
        python_exe=f"/opt/platformio-{seed}/penv/bin/python",
        core_dir=f"/opt/platformio-{seed}",
        esptool_path=(
            f"/opt/platformio-{seed}/packages/tool-esptoolpy/esptool.py"
        ),
        esptool_version="4.11.0",
        esptool_sha256=seed * 64,
    )


def installed_identity(role: str, mac: str) -> BoardIdentity:
    scanner = role.startswith("scanner")
    return BoardIdentity(
        role=role,
        port=f"/dev/cu.usbmodem-{role}",
        chip="ESP32-S3",
        mac=mac,
        flash_size=FLASH_SIZE,
        secure_boot_enabled=False,
        flash_encryption_enabled=False,
        installed_target=(
            "scanner-s3-combo-fof_badge" if scanner
            else "uplink-s3-fof_badge"
        ),
        installed_project=(
            "fof_badge_scanner" if scanner else "fof_badge_uplink"
        ),
        installed_hardware="seeed_xiao_esp32s3",
        installed_version="0.67.2-badge-defcon34",
        installed_role=role,
        installed_partition_sha256=canonical_partition_sha256(
            LEGACY_PARTITIONS["scanner" if scanner else "uplink"]
        ),
        updater_admission_evidence_sha256=PINNED_UPDATER_ADMISSION_SHA256,
        xiao_sense_sd_attached=False,
    )


def make_backup(
    tmp_path: Path,
    role: str,
    kind: str,
    mac: str,
    *,
    fill: int = 0xA5,
) -> BackupRecord:
    directory = tmp_path / kind / role
    full = private_file(directory / f"{role}-{mac}-full.bin", bytes([fill]) * FLASH_SIZE)
    nvs_bytes = bytes([fill]) * 0x6000
    nvs = private_file(directory / f"{role}-{mac}-nvs.bin", nvs_bytes)
    table = private_file(directory / f"{role}-{mac}-partition.bin", bytes([fill]) * 0x1000)
    return BackupRecord(
        role=role,
        kind=kind,
        mac=mac,
        full_path=str(full.resolve()),
        full_size=FLASH_SIZE,
        full_sha256=hashlib.sha256(full.read_bytes()).hexdigest(),
        nvs_path=str(nvs.resolve()),
        nvs_size=0x6000,
        nvs_sha256=hashlib.sha256(nvs_bytes).hexdigest(),
        partition_path=str(table.resolve()),
        partition_size=0x1000,
        partition_sha256=hashlib.sha256(table.read_bytes()).hexdigest(),
        decoded_partition_sha256=canonical_partition_sha256(
            LEGACY_PARTITIONS["uplink" if role == "uplink" else "scanner"]
            if kind == "original"
            else BACKEND_PARTITIONS["uplink" if role == "uplink" else "scanner"]
        ),
        toolchain_sha256=toolchain().sha256,
    )


def backend_identity(role: str, mac: str, *, boot_id: int = 100) -> dict[str, object]:
    scanner = role.startswith("scanner")
    result: dict[str, object] = {
        "target": "scanner-s3-combo-backend" if scanner else "uplink-s3-backend",
        "project": "fof_backend_scanner" if scanner else "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "version": "0.1.0-backend",
        "mac": mac,
        "boot_id": boot_id,
        "nvs_erased": False,
        "ota_state": "valid",
        "partition_sha256": canonical_partition_sha256(
            BACKEND_PARTITIONS["scanner" if scanner else "uplink"]
        ),
    }
    if scanner:
        result["uart_ingress"] = True
    else:
        result.update(
            device_id="uplink_CB77A4",
            config_state="loaded",
            config_generation=9,
            auto_update_enabled=False,
            uart0_started=True,
            uart1_started=True,
            network_state="ap",
        )
    return result


def ready_state(tmp_path: Path) -> CanaryState:
    state = CanaryState(toolchain=toolchain())
    identities = {
        "scanner0": installed_identity("scanner0", "AA:00:00:00:00:01"),
        "scanner1": installed_identity("scanner1", "AA:00:00:00:00:02"),
        "uplink": installed_identity("uplink", "AA:00:00:00:00:03"),
    }
    for identity in identities.values():
        state.record_installed_evidence(identity)
    for identity in identities.values():
        state.record_inventory(
            identity,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    for role in BOARD_ROLES:
        state.record_backup(
            role,
            "original",
            make_backup(tmp_path, role, "original", identities[role].mac),
        )
    state.record_original_uplink_quiesced(
        port=identities["uplink"].port,
        mac=identities["uplink"].mac,
        toolchain_sha256=toolchain().sha256,
        now=90,
    )
    return state


def test_backup_reads_full_flash_and_nvs_without_writing(tmp_path: Path):
    commands = build_backup_commands(
        esptool=Path("/opt/esptool.py"),
        port="/dev/cu.usbmodem1101",
        output_dir=tmp_path,
        role="scanner0",
        flash_size=0x800000,
    )
    joined = " ".join(" ".join(command) for command in commands)
    assert "read_flash 0x0 0x800000" in joined
    assert joined.count("read_flash 0x0 0x800000") == 2
    assert joined.count("read_flash 0x9000 0x6000") == 2
    assert "read_flash 0x8000 0x1000" in joined
    assert joined.count("--after no_reset") == len(commands)
    assert "write_flash" not in joined
    assert "erase_flash" not in joined
    assert all(command[0] != "/opt/esptool.py" for command in commands)


def test_initial_flash_never_writes_nvs_or_erases_all():
    command = build_initial_flash_command(
        esptool=Path("/opt/esptool.py"),
        port="/dev/cu.usbmodem1101",
        artifact_dir=Path("scanner-s3-combo-backend"),
    )
    pairs = list(zip(command, command[1:]))
    assert ("0x9000", "nvs.bin") not in pairs
    assert "erase_flash" not in command
    assert "erase_all" not in command
    assert "--after" in command and "no_reset" in command
    assert "--verify" in command
    assert command[command.index("--flash_mode") + 1] == "dio"
    assert command[command.index("--flash_freq") + 1] == "80m"
    assert command[command.index("--flash_size") + 1] == "8MB"
    assert command[-8:] == [
        "0x0", "scanner-s3-combo-backend/scanner-s3-combo-backend-bootloader.bin",
        "0x8000", "scanner-s3-combo-backend/scanner-s3-combo-backend-partition-table.bin",
        "0xf000", "scanner-s3-combo-backend/scanner-s3-combo-backend-ota-data-initial.bin",
        "0x20000", "scanner-s3-combo-backend/scanner-s3-combo-backend-firmware.bin",
    ]


def test_flash_range_proof_rejects_nvs_overlap_boundary_crossing_and_flash_end():
    valid = [(0x0, 0x7000), (0x8000, 0x1000), (0xF000, 0x2000), (0x20000, 0x100000)]
    validate_flash_ranges(valid)
    for mutated in (
        [(0x0, 0xA000), *valid[1:]],
        [valid[0], (0x8000, 0x2000), *valid[2:]],
        [*valid[:3], (0x210000, 0x20000)],
        [*valid[:3], (0x7F0000, 0x20000)],
    ):
        with pytest.raises(CanaryReleaseError):
            validate_flash_ranges(mutated)


@pytest.mark.parametrize("bad", ["--force", "--erase-all", "--encrypt", "--encrypt-files", "--ignore-flash-encryption-efuse-setting"])
def test_command_builders_reject_dangerous_operator_options(bad: str):
    with pytest.raises(CanarySecurityError):
        build_initial_flash_command(
            Path("/opt/esptool.py"),
            "/dev/cu.usbmodem1101",
            Path("scanner-s3-combo-backend"),
            extra_args=[bad],
        )


def test_fail_closed_inventory_requires_all_fields_unique_ports_and_no_security(tmp_path: Path):
    state = CanaryState(toolchain=toolchain())
    first = installed_identity("scanner0", "AA:00:00:00:00:01")
    state.record_installed_evidence(first)
    with pytest.raises(CanaryOrderError, match="installed evidence"):
        state.record_inventory(first)
    for role, mac in (
        ("scanner1", "AA:00:00:00:00:02"),
        ("uplink", "AA:00:00:00:00:03"),
    ):
        state.record_installed_evidence(installed_identity(role, mac))
    state.record_inventory(
        first,
        lite_sensor_confirmed=True,
        no_sd_expansion_confirmed=True,
    )
    duplicate_port = installed_identity("scanner1", "AA:00:00:00:00:02")
    object.__setattr__(duplicate_port, "port", first.port)
    with pytest.raises(CanaryInventoryError, match="port"):
        state.record_inventory(
            duplicate_port,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    encrypted = installed_identity("scanner1", "AA:00:00:00:00:02")
    object.__setattr__(encrypted, "flash_encryption_enabled", True)
    with pytest.raises(CanaryInventoryError, match="encryption"):
        state.record_inventory(
            encrypted,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    sd = installed_identity("scanner1", "AA:00:00:00:00:02")
    object.__setattr__(sd, "xiao_sense_sd_attached", True)
    with pytest.raises(CanaryInventoryError, match="GPIO3"):
        state.record_inventory(
            sd,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )


def test_badge_identity_alone_never_authorizes_lite_write(tmp_path: Path):
    """A source-audited .67.2 identity is not physical proof of a Lite sensor."""
    state = CanaryState(toolchain=toolchain())
    identities = [
        installed_identity("scanner0", "AA:00:00:00:00:01"),
        installed_identity("scanner1", "AA:00:00:00:00:02"),
        installed_identity("uplink", "AA:00:00:00:00:03"),
    ]
    for identity in identities:
        state.record_installed_evidence(identity)
    for identity in identities:
        with pytest.raises(CanaryInventoryError, match="no-screen Lite sensor"):
            state.record_inventory(
                identity,
                lite_sensor_confirmed=False,
                no_sd_expansion_confirmed=True,
            )


def test_unpinned_updater_admission_hash_cannot_authorize_inventory():
    state = CanaryState(toolchain=toolchain())
    identities = [
        installed_identity("scanner0", "AA:00:00:00:00:01"),
        installed_identity("scanner1", "AA:00:00:00:00:02"),
        installed_identity("uplink", "AA:00:00:00:00:03"),
    ]
    object.__setattr__(
        identities[0], "updater_admission_evidence_sha256", "9" * 64
    )
    with pytest.raises(CanaryInventoryError, match="pinned source-audited"):
        state.record_installed_evidence(identities[0])


def test_lite_and_no_sd_confirmations_are_persisted_and_challenge_bound(tmp_path: Path):
    state = ready_state(tmp_path)
    for role in BOARD_ROLES:
        assert state.boards[role].lite_sensor_confirmed is True
        assert state.boards[role].no_sd_expansion_confirmed is True
    challenge, _token = state.issue_challenge(
        role="scanner0", port="/dev/cu.usbmodem-scanner0",
        mac="AA:00:00:00:00:01", artifact_sha256="a" * 64,
        offsets_sha256="b" * 64, now=100,
    )
    assert challenge.lite_sensor_confirmed is True
    assert challenge.no_sd_expansion_confirmed is True


def test_no_flash_before_every_inventory_backup_and_quiescence(tmp_path: Path):
    state = CanaryState(toolchain=toolchain())
    identities = [
        installed_identity("scanner0", "AA:00:00:00:00:01"),
        installed_identity("scanner1", "AA:00:00:00:00:02"),
        installed_identity("uplink", "AA:00:00:00:00:03"),
    ]
    for identity in identities:
        state.record_installed_evidence(identity)
    for identity in identities:
        state.record_inventory(
            identity,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    for identity in identities[:2]:
        state.record_backup(
            identity.role, "original",
            make_backup(tmp_path, identity.role, "original", identity.mac),
        )
    with pytest.raises(CanaryOrderError, match="uplink"):
        state.issue_challenge(
            role="scanner0", port=identities[0].port, mac=identities[0].mac,
            artifact_sha256="a" * 64, offsets_sha256="b" * 64, now=100,
        )


def test_original_uplink_backup_order_and_never_restart(tmp_path: Path):
    state = CanaryState(toolchain=toolchain())
    ids = {
        role: installed_identity(role, f"AA:00:00:00:00:0{index}")
        for index, role in enumerate(BOARD_ROLES, start=1)
    }
    for identity in ids.values():
        state.record_installed_evidence(identity)
    for identity in ids.values():
        state.record_inventory(
            identity,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    with pytest.raises(CanaryOrderError, match="scanner0"):
        state.record_backup("uplink", "original", make_backup(
            tmp_path, "uplink", "original", ids["uplink"].mac,
        ))
    state.record_backup("scanner0", "original", make_backup(
        tmp_path, "scanner0", "original", ids["scanner0"].mac,
    ))
    with pytest.raises(CanaryOrderError, match="scanner1"):
        state.record_backup("uplink", "original", make_backup(
            tmp_path / "again", "uplink", "original", ids["uplink"].mac,
        ))


def test_uplink_is_refused_until_both_scanners_are_verified(tmp_path: Path):
    state = ready_state(tmp_path)
    state.record_provisional_backend_identity(
        "scanner0", backend_identity("scanner0", "AA:00:00:00:00:01"),
    )
    with pytest.raises(CanaryOrderError, match="scanner1"):
        state.issue_challenge(
            role="uplink", port="/dev/cu.usbmodem-uplink",
            mac="AA:00:00:00:00:03", artifact_sha256="a" * 64,
            offsets_sha256="b" * 64, now=100,
        )


def test_scanner1_cannot_be_skipped(tmp_path: Path):
    state = ready_state(tmp_path)
    with pytest.raises(CanaryOrderError, match="scanner0"):
        state.issue_challenge(
            role="scanner1", port=state.boards["scanner1"].inventory.port,
            mac=state.boards["scanner1"].inventory.mac,
            artifact_sha256="a" * 64, offsets_sha256="b" * 64, now=100,
        )


def test_flash_challenge_is_one_use_bound_and_reboot_invalidates_all(tmp_path: Path):
    state = ready_state(tmp_path)
    challenge, token = state.issue_challenge(
        role="scanner0", port="/dev/cu.usbmodem-scanner0",
        mac="AA:00:00:00:00:01", artifact_sha256="a" * 64,
        offsets_sha256="b" * 64, now=100,
    )
    assert state.consume_challenge(challenge.challenge_id, token, now=101)
    with pytest.raises(CanaryApprovalError, match="consumed"):
        state.consume_challenge(challenge.challenge_id, token, now=102)

    state = ready_state(tmp_path / "second")
    challenge, token = state.issue_challenge(
        role="scanner0", port="/dev/cu.usbmodem-scanner0",
        mac="AA:00:00:00:00:01", artifact_sha256="a" * 64,
        offsets_sha256="b" * 64, now=100,
    )
    state.note_original_uplink_application_reboot(boot_id=55, now=101)
    with pytest.raises(CanaryApprovalError, match="state generation|quiescent|invalidated"):
        state.consume_challenge(challenge.challenge_id, token, now=102)


def test_preconsume_uplink_quiescence_probe_does_not_invalidate_challenge_generation(
    tmp_path: Path,
):
    state = ready_state(tmp_path)
    challenge, _token = state.issue_challenge(
        role="scanner0", port="/dev/cu.usbmodem-scanner0",
        mac="AA:00:00:00:00:01", artifact_sha256="a" * 64,
        offsets_sha256="b" * 64, now=100,
    )

    def runner(command, **_kwargs):
        operation = command[-1]
        if operation == "get_security_info":
            output = (
                "Chip is ESP32-S3 (revision v0.2)\n"
                "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
            )
        elif operation == "flash_id":
            output = "Detected flash size: 8MB\n"
        else:
            assert operation == "read_mac"
            output = "MAC: AA:00:00:00:00:03\n"
        return subprocess.CompletedProcess(command, 0, stdout=output)

    verify_original_uplink_quiescence(
        state, receipt=toolchain(), runner=runner, persist=False
    )
    assert state.generation == challenge.state_generation

@pytest.mark.parametrize("mutation", ["port", "mac", "artifact", "offsets", "toolchain", "expiry"])
def test_changed_live_challenge_binding_fails(tmp_path: Path, mutation: str):
    state = ready_state(tmp_path)
    challenge, token = state.issue_challenge(
        role="scanner0", port="/dev/cu.usbmodem-scanner0",
        mac="AA:00:00:00:00:01", artifact_sha256="a" * 64,
        offsets_sha256="b" * 64, now=100,
    )
    kwargs: dict[str, object] = {"now": 101}
    if mutation == "port": kwargs["port"] = "/dev/cu.swapped"
    if mutation == "mac": kwargs["mac"] = "AA:00:00:00:00:99"
    if mutation == "artifact": kwargs["artifact_sha256"] = "c" * 64
    if mutation == "offsets": kwargs["offsets_sha256"] = "d" * 64
    if mutation == "toolchain": kwargs["toolchain"] = toolchain("b")
    if mutation == "expiry": kwargs["now"] = 401
    with pytest.raises(CanaryApprovalError):
        state.consume_challenge(challenge.challenge_id, token, **kwargs)


def test_failure_blocks_every_other_write_and_restore_source_is_exact(tmp_path: Path):
    state = ready_state(tmp_path)
    state.record_flash_failure("scanner0", phase="initial", reason="write failed")
    with pytest.raises(CanaryOrderError, match="scanner0"):
        state.issue_challenge(
            role="scanner1", port="/dev/cu.usbmodem-scanner1",
            mac="AA:00:00:00:00:02", artifact_sha256="a" * 64,
            offsets_sha256="b" * 64, now=100,
        )
    with pytest.raises(CanaryOrderError, match="original"):
        state.issue_restore_challenge(
            "scanner0", source="backend-baseline",
            full_backup_sha256="c" * 64, now=100,
        )
    original = state.boards["scanner0"].backups["original"]
    challenge, token = state.issue_restore_challenge(
        "scanner0", source="original",
        full_backup_sha256=original.full_sha256, now=100,
    )
    command = state.authorize_restore(challenge.challenge_id, token, now=101)
    assert "--verify" in command
    assert command[-2:] == ["0x0", original.full_path]


def test_restore_command_cannot_select_another_role_or_non_8mb_backup(tmp_path: Path):
    backup = make_backup(tmp_path, "scanner0", "original", "AA:00:00:00:00:01")
    command = build_restore_command(
        Path("/opt/esptool.py"), "/dev/cu.usbmodem-scanner0", backup,
    )
    assert command[-2:] == ["0x0", backup.full_path]
    object.__setattr__(backup, "full_size", FLASH_SIZE - 1)
    with pytest.raises(CanaryBackupError, match="8 MB"):
        build_restore_command(Path("/opt/esptool.py"), "/dev/x", backup)


def test_backup_rehash_rejects_length_digest_permissions_and_existing_kind(tmp_path: Path):
    state = ready_state(tmp_path)
    backup = state.boards["scanner0"].backups["original"]
    Path(backup.full_path).chmod(0o644)
    with pytest.raises(CanaryBackupError, match="0600"):
        state.verify_backup("scanner0", "original")
    Path(backup.full_path).chmod(0o600)
    Path(backup.full_path).write_bytes(b"changed")
    with pytest.raises(CanaryBackupError, match="size|SHA"):
        state.verify_backup("scanner0", "original")
    with pytest.raises(CanaryBackupError, match="already"):
        state.record_backup("scanner0", "original", backup)


def test_state_persistence_is_private_atomic_and_token_hash_only(tmp_path: Path):
    path = tmp_path / ".canary" / "state.json"
    state = CanaryState.create(path, toolchain=toolchain())
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    loaded = CanaryState.load(path)
    assert loaded.schema == 1
    assert "token_urlsafe" not in path.read_text(encoding="utf-8")


def test_redaction_removes_secret_keys_recursively():
    payload = {
        "password": "wifi",
        "Authorization": "Bearer x",
        "nested": {"api_key": "abc", "safe": 7},
        "list": [{"cookie": "sid=1", "value": "ok"}],
    }
    redacted = redact_secrets(payload)
    rendered = json.dumps(redacted)
    for secret in ("wifi", "Bearer x", "abc", "sid=1"):
        assert secret not in rendered
    assert redacted["nested"]["safe"] == 7


def partition_binary(entries: tuple[tuple[str, str, str, int, int], ...]) -> bytes:
    type_map = {"app": 0x00, "data": 0x01}
    subtype_map = {
        "ota_0": 0x10, "ota_1": 0x11, "nvs": 0x02, "ota": 0x00,
        "phy": 0x01, "spiffs": 0x82, "fat": 0x81, "0x40": 0x40,
    }
    output = bytearray()
    for label, kind, subtype, offset, size in entries:
        output += struct.pack(
            "<HBBII16sI", 0x50AA, type_map[kind], subtype_map[subtype],
            offset, size, label.encode("ascii") + bytes(16 - len(label)), 0,
        )
    output += bytes([0xFF]) * (0x1000 - len(output))
    return bytes(output)


def test_legacy_and_backend_partition_tables_are_distinct_and_exact():
    legacy = decode_partition_table(partition_binary(LEGACY_PARTITIONS["uplink"]))
    backend = decode_partition_table(partition_binary(BACKEND_PARTITIONS["uplink"]))
    assert any(row[0] == "fw_scanner_s3" for row in legacy)
    assert not any(row[0] == "fw_scanner_be" for row in legacy)
    assert any(row[0] == "fw_scanner_be" for row in backend)
    assert canonical_partition_sha256(legacy) != canonical_partition_sha256(backend)
    with pytest.raises(CanaryReleaseError, match="partition"):
        decode_partition_table(b"not a partition table")


def make_release(tmp_path: Path, target: str) -> tuple[Path, Path]:
    targets = ("scanner-s3-combo-backend", "uplink-s3-backend")
    index_targets = {}
    for release_target in targets:
        kind = "scanner" if release_target.startswith("scanner") else "uplink"
        directory = tmp_path / "web-flasher" / "firmware" / release_target
        directory.mkdir(parents=True)
        parts = []
        for logical, offset, size in (
            ("bootloader", 0x0, 128),
            ("partition-table", 0x8000, 0x1000),
            ("ota-data-initial", 0xF000, 0x2000),
            ("firmware", 0x20000, 512),
        ):
            name = f"{release_target}-{logical}.bin"
            data = (
                partition_binary(BACKEND_PARTITIONS[kind])
                if logical == "partition-table"
                else bytes([len(parts) + 1]) * size
            )
            private_file(directory / name, data)
            parts.append({
                "name": name,
                "path": f"{release_target}/{name}",
                "offset": offset,
                "size": size,
                "sha256": hashlib.sha256(data).hexdigest(),
                "crc32": __import__("zlib").crc32(data) & 0xFFFFFFFF,
            })
        index_targets[release_target] = {
            "kind": kind,
            "target": release_target,
            "project": "fof_backend_scanner" if kind == "scanner" else "fof_backend_uplink",
            "hardware": "seeed_xiao_esp32s3",
            "identity_crc32": 0x9DD382FF if kind == "scanner" else 0xF08BCDE4,
            "partition_capacity": 0x200000,
            "parts": parts,
        }
    index = {
        "schema": 1,
        "version": "0.1.0-backend",
        "targets": index_targets,
    }
    index_path = private_file(tmp_path / "index.json", json.dumps(index).encode())
    return index_path, tmp_path / "web-flasher" / "firmware" / target


@pytest.mark.parametrize(
    ("role", "target"),
    [
        ("scanner0", "scanner-s3-combo-backend"),
        ("uplink", "uplink-s3-backend"),
    ],
)
def test_committed_release_index_resolves_real_artifact_directory(
    role: str,
    target: str,
):
    backend_firmware = Path(__file__).resolve().parents[2]
    index = backend_firmware / "release/backend-release-index.json"
    directory = backend_firmware / "web-flasher/firmware" / target

    artifact = verify_release_artifact(index, directory, role=role)

    assert artifact.target == target
    assert [part["path"] for part in artifact.parts] == [
        f"{target}/{part['name']}" for part in artifact.parts
    ]


def test_release_artifact_is_exact_kind_verified_and_cannot_escape(tmp_path: Path):
    index, scanner_dir = make_release(tmp_path, "scanner-s3-combo-backend")
    artifact = verify_release_artifact(index, scanner_dir, role="scanner0")
    assert artifact.target == "scanner-s3-combo-backend"
    with pytest.raises(CanaryReleaseError, match="scanner"):
        verify_release_artifact(index, scanner_dir, role="uplink")
    parsed = json.loads(index.read_text())
    parsed["targets"]["scanner-s3-combo-backend"]["parts"][-1]["path"] = "../escape.bin"
    index.write_text(json.dumps(parsed))
    index.chmod(0o600)
    with pytest.raises(CanaryReleaseError, match="path"):
        verify_release_artifact(index, scanner_dir, role="scanner0")


def test_unverified_release_fails_before_subprocess(tmp_path: Path):
    index, directory = make_release(tmp_path, "scanner-s3-combo-backend")
    (directory / "scanner-s3-combo-backend-firmware.bin").write_bytes(b"tamper")
    started: list[list[str]] = []
    with pytest.raises(CanaryReleaseError, match="digest|size"):
        verify_release_artifact(index, directory, role="scanner0")
    assert started == []


def inventory_runner(calls: list[list[str]]):
    macs = {
        "/dev/cu.usbmodem-scanner0": "AA:00:00:00:00:01",
        "/dev/cu.usbmodem-scanner1": "AA:00:00:00:00:02",
        "/dev/cu.usbmodem-uplink": "AA:00:00:00:00:03",
    }

    def run(command, **_kwargs):
        calls.append(list(command))
        operation = command[-1]
        if operation == "get_security_info":
            output = (
                "Chip is ESP32-S3 (revision v0.2)\n"
                "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
            )
        elif operation == "flash_id":
            output = "Detected flash size: 8MB\n"
        elif operation == "read_mac":
            port = command[command.index("--port") + 1]
            output = f"MAC: {macs[port]}\n"
        else:
            output = "ok\n"
        return subprocess.CompletedProcess(command, 0, stdout=output)

    return run


@pytest.mark.parametrize("part_index", range(4))
def test_initial_flash_rejects_each_staged_part_mutation_before_write(
    tmp_path: Path, part_index: int,
):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    calls: list[list[str]] = []
    runner = inventory_runner(calls)
    receipt_path = tmp_path / ".canary" / "receipts" / "scanner0.json"
    challenge, token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=receipt_path,
        runner=runner,
        now=100,
    )
    assert challenge.staged_parts is not None
    assert len(challenge.staged_parts) == 4
    staged = Path(challenge.staged_parts[part_index].path)
    staged.write_bytes(b"mutated-after-approval")
    staged.chmod(0o600)

    with pytest.raises(CanaryReleaseError, match="staged"):
        execute_initial_flash(
            state,
            role="scanner0",
            artifact=artifact,
            challenge_id=challenge.challenge_id,
            token=token,
            receipt=toolchain(),
            runner=runner,
            now=101,
        )

    assert not any("write_flash" in command for command in calls)
    assert not staged.parent.exists()
    assert not receipt_path.exists()
    assert state.challenges[challenge.challenge_id].consumed_at == 101
    assert state.boards["scanner0"].status != "restore_required"


def test_initial_flash_writes_only_frozen_challenge_parts(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    calls: list[list[str]] = []
    base_runner = inventory_runner(calls)
    inherited_bytes: list[bytes] = []
    inherited_fds: tuple[int, ...] = ()
    challenge = None

    def runner(command, **kwargs):
        nonlocal inherited_fds
        if "write_flash" in command:
            calls.append(list(command))
            inherited_fds = tuple(kwargs.get("pass_fds", ()))
            assert challenge is not None
            for part in challenge.staged_parts or ():
                staged = Path(part.path)
                staged.unlink()
                staged.write_bytes(b"same-uid-path-replacement")
                staged.chmod(0o600)
            inherited_bytes.extend(
                Path(path).read_bytes() for path in command[-7::2]
            )
            return subprocess.CompletedProcess(command, 0, stdout="write ok\n")
        if "read_flash" in command:
            calls.append(list(command))
            output = Path(command[-1])
            output.write_bytes(bytes([0xA5]) * 0x6000)
            output.chmod(0o600)
            return subprocess.CompletedProcess(command, 0, stdout="read ok\n")
        return base_runner(command, **kwargs)

    challenge, token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=tmp_path / ".canary" / "receipts" / "scanner0.json",
        runner=runner,
        now=100,
    )
    staged_directory = Path(challenge.staging_directory or "")
    execute_initial_flash(
        state,
        role="scanner0",
        artifact=artifact,
        challenge_id=challenge.challenge_id,
        token=token,
        receipt=toolchain(),
        runner=runner,
        now=101,
    )

    writes = [command for command in calls if "write_flash" in command]
    assert len(writes) == 1
    write = writes[0]
    assert tuple(write[-7::2]) == tuple(
        f"/dev/fd/{descriptor}" for descriptor in inherited_fds
    )
    expected_bytes = [
        (directory / part.name).read_bytes()
        for part in sorted(challenge.staged_parts or (), key=lambda item: item.offset)
    ]
    assert inherited_bytes == expected_bytes
    assert all(
        Path(part.path).parent == staged_directory
        for part in challenge.staged_parts or ()
    )
    write_index = calls.index(write)
    assert calls[write_index - 1][-1] == "read_mac"
    assert calls[write_index - 1][
        calls[write_index - 1].index("--port") + 1
    ] == "/dev/cu.usbmodem-scanner0"
    assert not staged_directory.exists()


def test_staging_cleanup_never_follows_replaced_challenge_symlink(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    calls: list[list[str]] = []
    receipt_path = tmp_path / ".canary/receipts/scanner0.json"
    challenge, _token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=receipt_path,
        runner=inventory_runner(calls),
        now=100,
    )
    staging = Path(challenge.staging_directory or "")
    outside = tmp_path / "must-survive"
    outside.mkdir()
    sentinel = private_file(outside / "sentinel.bin", b"do not delete")
    for child in staging.iterdir():
        child.unlink()
    staging.rmdir()
    staging.symlink_to(outside, target_is_directory=True)

    state.invalidate_challenge(challenge.challenge_id, now=101)

    assert sentinel.read_bytes() == b"do not delete"
    assert not os.path.lexists(staging)
    assert not receipt_path.exists()


def test_staging_creation_rejects_symlinked_challenges_parent(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    canary = tmp_path / ".canary"
    canary.mkdir(mode=0o700)
    outside = tmp_path / "outside-staging"
    outside.mkdir()
    (canary / "challenges").symlink_to(outside, target_is_directory=True)
    calls: list[list[str]] = []

    with pytest.raises(CanarySecurityError, match="exact private"):
        issue_initial_flash_challenge(
            state,
            role="scanner0",
            artifact=artifact,
            receipt=toolchain(),
            output=canary / "receipts/scanner0.json",
            runner=inventory_runner(calls),
            now=100,
        )

    assert list(outside.iterdir()) == []
    assert not any("write_flash" in command for command in calls)


def test_stale_generation_cleans_frozen_parts_and_receipt(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    calls: list[list[str]] = []
    receipt_path = tmp_path / ".canary/receipts/scanner0.json"
    challenge, token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=receipt_path,
        runner=inventory_runner(calls),
        now=100,
    )
    staging = Path(challenge.staging_directory or "")
    state._touch()

    with pytest.raises(CanaryApprovalError, match="generation"):
        state.consume_challenge(challenge.challenge_id, token, now=101)

    assert not staging.exists()
    assert not receipt_path.exists()


def test_receipt_cleanup_fails_closed_if_parent_is_replaced_by_symlink(
    tmp_path: Path,
):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    canary = tmp_path / ".canary"
    receipt_path = canary / "receipts/scanner0.json"
    challenge, _token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=receipt_path,
        runner=inventory_runner([]),
        now=100,
    )
    bound_parent = canary / "receipts-bound"
    receipt_path.parent.rename(bound_parent)
    outside = tmp_path / "outside-receipts"
    outside.mkdir(mode=0o700)
    sentinel = private_file(outside / receipt_path.name, b"must survive")
    receipt_path.parent.symlink_to(outside, target_is_directory=True)

    with pytest.raises(CanarySecurityError, match="exact private"):
        state.invalidate_challenge(challenge.challenge_id, now=101)

    assert sentinel.read_bytes() == b"must survive"
    assert Path(challenge.staging_directory or "").exists()


def test_initial_challenge_save_failure_rolls_back_receipt_stage_and_hash(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    state = ready_state(tmp_path / "backups")
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    original_save = state.save
    failed = False

    def fail_after_receipt(*, initial: bool = False):
        nonlocal failed
        if state.challenge_receipts and not failed:
            failed = True
            raise OSError("injected state fsync failure")
        return original_save(initial=initial)

    monkeypatch.setattr(state, "save", fail_after_receipt)
    canary = tmp_path / ".canary"
    with pytest.raises(OSError, match="injected"):
        issue_initial_flash_challenge(
            state,
            role="scanner0",
            artifact=artifact,
            receipt=toolchain(),
            output=canary / "receipts/scanner0.json",
            runner=inventory_runner([]),
            now=100,
        )

    assert failed is True
    assert state.challenges == {}
    assert state.challenge_hashes == {}
    assert state.challenge_receipts == {}
    assert state.challenge_receipt_bindings == {}
    assert not any((canary / "challenges").iterdir())
    assert not any((canary / "receipts").iterdir())


def test_load_retires_expired_challenge_receipt_and_staging(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    state_path = tmp_path / ".canary/state.json"
    state.state_path = str(state_path.resolve())
    state.save(initial=True)
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    receipt_path = state_path.parent / "receipts/scanner0.json"
    challenge, _token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=receipt_path,
        runner=inventory_runner([]),
        now=100,
    )
    staging = Path(challenge.staging_directory or "")

    loaded = CanaryState.load(state_path, now=401)

    retired = loaded.challenges[challenge.challenge_id]
    assert retired.consumed_at == 401
    assert retired.staging_directory is None
    assert retired.staged_parts is None
    assert challenge.challenge_id not in loaded.challenge_hashes
    assert challenge.challenge_id not in loaded.challenge_receipts
    assert challenge.challenge_id not in loaded.challenge_receipt_bindings
    assert not receipt_path.exists()
    assert not staging.exists()


def test_load_cleans_staging_left_after_consumed_challenge(tmp_path: Path):
    state = ready_state(tmp_path / "backups")
    state_path = tmp_path / ".canary/state.json"
    state.state_path = str(state_path.resolve())
    state.save(initial=True)
    index, directory = make_release(
        tmp_path / "release", "scanner-s3-combo-backend",
    )
    artifact = verify_release_artifact(index, directory, role="scanner0")
    challenge, token = issue_initial_flash_challenge(
        state,
        role="scanner0",
        artifact=artifact,
        receipt=toolchain(),
        output=state_path.parent / "receipts/scanner0.json",
        runner=inventory_runner([]),
        now=100,
    )
    staging = Path(challenge.staging_directory or "")
    state.consume_challenge(challenge.challenge_id, token, now=101)
    assert staging.exists()

    loaded = CanaryState.load(state_path, now=102)

    retired = loaded.challenges[challenge.challenge_id]
    assert retired.consumed_at == 101
    assert retired.staging_directory is None
    assert retired.staged_parts is None
    assert not staging.exists()


def test_provisional_requires_exact_backend_identity_mac_and_valid_direct_usb_ota():
    value = backend_identity("scanner0", "AA:00:00:00:00:01")
    assert validate_provisional_identity("scanner0", value, "AA:00:00:00:00:01")["boot_id"] == 100
    for field in ("target", "project", "hardware", "version", "uart_ingress"):
        broken = dict(value)
        broken.pop(field)
        with pytest.raises(CanaryInventoryError, match=field):
            validate_provisional_identity("scanner0", broken, "AA:00:00:00:00:01")
    pending = dict(value, ota_state="pending_verify")
    with pytest.raises(CanaryInventoryError, match="valid"):
        validate_provisional_identity("scanner0", pending, "AA:00:00:00:00:01")


def scanner_health(mac: str, boot_id: int, role: str) -> dict[str, object]:
    return {
        "target": "scanner-s3-combo-backend", "mac": mac,
        "boot_id": boot_id, "nvs_erased": False, "role": role,
        "command_ingress_boot_id": boot_id, "radio_healthy": True,
        "rollback_clear": True,
    }


def uplink_health(boot_id: int = 300) -> dict[str, object]:
    return {
        "target": "uplink-s3-backend", "mac": "AA:00:00:00:00:03",
        "boot_id": boot_id, "device_id": "uplink_CB77A4",
        "config_state": "loaded", "config_generation": 9,
        "nvs_loaded": True, "nvs_erased": False,
        "auto_update_enabled": False, "uart0_started": True,
        "uart1_started": True, "coordinator_started": True,
        "network_state": "ap", "rollback_clear": True,
    }


def test_final_health_requires_distinct_scanner_roles_same_boots_and_uplink_state():
    provisional = {
        "scanner0": backend_identity("scanner0", "AA:00:00:00:00:01", boot_id=100),
        "scanner1": backend_identity("scanner1", "AA:00:00:00:00:02", boot_id=200),
        "uplink": backend_identity("uplink", "AA:00:00:00:00:03", boot_id=300),
    }
    health = {
        "scanner0": scanner_health("AA:00:00:00:00:01", 100, "ble_primary"),
        "scanner1": scanner_health("AA:00:00:00:00:02", 200, "wifi_primary"),
        "uplink": uplink_health(),
    }
    validate_final_health_set(health, provisional, device_id="uplink_CB77A4")
    health["scanner1"] = scanner_health("AA:00:00:00:00:02", 200, "ble_primary")
    with pytest.raises(CanaryInventoryError, match="roles"):
        validate_final_health_set(health, provisional, device_id="uplink_CB77A4")


def test_backend_backup_reboot_sequence_requires_new_boot_and_full_topology(
    tmp_path: Path,
):
    state = ready_state(tmp_path / "original")
    boots = {"scanner0": 100, "scanner1": 200, "uplink": 300}
    for role in BOARD_ROLES:
        state.record_provisional_backend_identity(
            role,
            backend_identity(
                role, state.boards[role].inventory.mac, boot_id=boots[role],
            ),
        )
    state.record_final_backend_health_set(
        {
            "scanner0": scanner_health(
                state.boards["scanner0"].inventory.mac, boots["scanner0"],
                "ble_primary",
            ),
            "scanner1": scanner_health(
                state.boards["scanner1"].inventory.mac, boots["scanner1"],
                "wifi_primary",
            ),
            "uplink": uplink_health(boots["uplink"]),
        },
        device_id="uplink_CB77A4",
    )
    state.record_catalog_preflight("e" * 64, now=90)

    for sequence, role in enumerate(BOARD_ROLES, start=1):
        state.record_backup(
            role,
            "backend-baseline",
            make_backup(
                tmp_path / "baseline", role, "backend-baseline",
                state.boards[role].inventory.mac, fill=0x5A,
            ),
        )
        state.begin_backend_backup_reboot(role, now=100 + sequence)
        assert state.post_backup_reboot["role"] == role
        assert state.boards[role].provisional is None
        assert all(board.final_health is None for board in state.boards.values())

        with pytest.raises(CanaryInventoryError, match="boot_id.*change"):
            state.record_provisional_backend_identity(
                role,
                backend_identity(
                    role, state.boards[role].inventory.mac,
                    boot_id=boots[role],
                ),
            )

        boots[role] += 1
        state.record_provisional_backend_identity(
            role,
            backend_identity(
                role, state.boards[role].inventory.mac, boot_id=boots[role],
            ),
        )
        assert state.post_backup_reboot["provisional_reverified"] is True
        if role != "uplink":
            next_role = BOARD_ROLES[sequence]
            with pytest.raises(CanaryOrderError, match="final health"):
                state.record_backup(
                    next_role,
                    "backend-baseline",
                    make_backup(
                        tmp_path / "too-early", next_role,
                        "backend-baseline",
                        state.boards[next_role].inventory.mac, fill=0x5A,
                    ),
                )

        state.record_final_backend_health_set(
            {
                "scanner0": scanner_health(
                    state.boards["scanner0"].inventory.mac,
                    boots["scanner0"], "ble_primary",
                ),
                "scanner1": scanner_health(
                    state.boards["scanner1"].inventory.mac,
                    boots["scanner1"], "wifi_primary",
                ),
                "uplink": uplink_health(boots["uplink"]),
            },
            device_id="uplink_CB77A4",
        )
        assert state.post_backup_reboot is None
        assert all(board.final_health is not None for board in state.boards.values())

    assert all(
        "backend-baseline" in state.boards[role].backups for role in BOARD_ROLES
    )


def test_json_evidence_rejects_duplicate_keys_and_multiple_records():
    with pytest.raises(CanaryInventoryError, match="duplicate"):
        parse_evidence_line('FOF_BACKEND_BOOT {"mac":"A","mac":"B"}', "FOF_BACKEND_BOOT")
    with pytest.raises(CanaryInventoryError, match="one"):
        parse_evidence_line(
            'FOF_BACKEND_BOOT {"mac":"A"}\nFOF_BACKEND_BOOT {"mac":"A"}',
            "FOF_BACKEND_BOOT",
        )


def valid_probe(component: str = "scanner0") -> dict[str, object]:
    scanner = component.startswith("scanner")
    slot = 0 if component == "scanner0" else 1 if component == "scanner1" else -1
    mac = "AA:00:00:00:00:01" if component == "scanner0" else "AA:00:00:00:00:02" if component == "scanner1" else "AA:00:00:00:00:03"
    return {
        "schema": 1, "operation_id": 1, "mode": "probe",
        "component": component, "component_slot": slot,
        "uplink_mac": "AA:00:00:00:00:03",
        "expected_target_mac": mac, "actual_target_mac": mac,
        "expected_target_boot_id": 100, "actual_target_boot_id": 100,
        "expected_topology_generation": 7 if scanner else 0,
        "actual_topology_generation": 7 if scanner else 0,
        "catalog_name": "scanner-s3-combo-backend" if scanner else "uplink-s3-backend",
        "target": "scanner-s3-combo-backend" if scanner else "uplink-s3-backend",
        "project": "fof_backend_scanner" if scanner else "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3", "version": "0.1.0-backend",
        "sha256": "d" * 64, "crc32": 0, "size": 1000,
        "partition_capacity": 0x200000, "allow_same_version": True,
        "decision": "admit", "complete_image_validated": True,
        "image_writes_before": 7, "image_writes_after": 7,
        "boot_id_before": 100, "boot_id_after": 100,
        "rollback_clear": True, "converged": False,
    }


@pytest.mark.parametrize("decision", [
    "admit", "no_update", "reject_identity", "reject_version",
    "reject_digest", "reject_size", "reject_capacity", "reject_busy",
    "applied", "failed",
])
def test_ota_parser_accepts_only_exact_decision_spellings(decision: str):
    payload = valid_probe()
    payload["decision"] = decision
    assert parse_ota_evidence(payload).decision == decision
    payload["decision"] = decision.upper()
    with pytest.raises(CanaryApprovalError, match="decision"):
        parse_ota_evidence(payload)


@pytest.mark.parametrize("field", [
    "actual_target_mac", "actual_target_boot_id",
    "actual_topology_generation", "crc32", "complete_image_validated",
])
def test_ota_probe_rejects_missing_or_mismatched_binding(field: str):
    payload = valid_probe()
    if field == "actual_target_mac": payload[field] = "AA:00:00:00:00:99"
    elif field == "actual_target_boot_id": payload[field] = 101
    elif field == "actual_topology_generation": payload[field] = 8
    elif field == "crc32": payload.pop(field)
    else: payload[field] = False
    with pytest.raises(CanaryApprovalError):
        parse_ota_evidence(payload)


def ota_ready_state(tmp_path: Path) -> CanaryState:
    state = ready_state(tmp_path)
    for role, boot in (("scanner0", 100), ("scanner1", 200), ("uplink", 300)):
        state.record_provisional_backend_identity(
            role,
            backend_identity(role, state.boards[role].inventory.mac, boot_id=boot),
        )
    state.record_final_backend_health_set(
        {
            "scanner0": scanner_health("AA:00:00:00:00:01", 100, "ble_primary"),
            "scanner1": scanner_health("AA:00:00:00:00:02", 200, "wifi_primary"),
            "uplink": uplink_health(),
        },
        device_id="uplink_CB77A4",
    )
    baseline_receipt = private_file(
        tmp_path / ".canary/catalog/baseline.json", b"baseline\n",
    )
    state.record_catalog_preflight(
        "e" * 64, now=10, evidence_path=baseline_receipt,
    )
    for role in BOARD_ROLES:
        state.record_backup(
            role, "backend-baseline",
            make_backup(tmp_path, role, "backend-baseline", state.boards[role].inventory.mac, fill=0x5A),
        )
    ota_receipt = private_file(
        tmp_path / ".canary/catalog/ota-scanner0-001.json", b"fresh ota\n",
    )
    state.record_ota_catalog_receipt(
        "f" * 64, now=95, evidence_path=ota_receipt,
    )
    return state


def test_stale_baseline_receipt_cannot_replace_fresh_post_backup_ota_receipt(
    tmp_path: Path,
):
    state = ready_state(tmp_path / "original")
    for role, boot in (("scanner0", 100), ("scanner1", 200), ("uplink", 300)):
        state.record_provisional_backend_identity(
            role,
            backend_identity(role, state.boards[role].inventory.mac, boot_id=boot),
        )
    state.record_final_backend_health_set(
        {
            "scanner0": scanner_health("AA:00:00:00:00:01", 100, "ble_primary"),
            "scanner1": scanner_health("AA:00:00:00:00:02", 200, "wifi_primary"),
            "uplink": uplink_health(),
        },
        device_id="uplink_CB77A4",
    )
    baseline_path = private_file(
        tmp_path / ".canary/catalog/baseline-100.json", b"baseline\n",
    )
    state.record_catalog_preflight(
        "b" * 64, now=100, evidence_path=baseline_path,
    )
    for role in BOARD_ROLES:
        state.record_backup(
            role, "backend-baseline",
            make_backup(
                tmp_path / "baseline", role, "backend-baseline",
                state.boards[role].inventory.mac, fill=0x5A,
            ),
        )
    probe = parse_ota_evidence(valid_probe())
    with pytest.raises(CanaryApprovalError, match="fresh OTA catalog"):
        state.record_ota_probe(probe, now=402)

    fresh_path = private_file(
        tmp_path / ".canary/catalog/ota-scanner0-401.json", b"fresh\n",
    )
    state.record_ota_catalog_receipt(
        "f" * 64, now=401, evidence_path=fresh_path,
    )
    recorded = state.record_ota_probe(probe, now=402)
    challenge, _token = state.issue_ota_challenge(
        component="scanner0",
        artifact_sha256=recorded.sha256,
        artifact_crc32=recorded.crc32,
        mode="newer-only",
        now=402,
    )
    assert challenge.offsets_sha256 == "f" * 64
    assert state.baseline_catalog_captured_at == 100
    assert state.ota_catalog_captured_at == 401


def test_ota_probe_is_read_only_and_apply_requires_one_use_challenge(tmp_path: Path):
    state = ota_ready_state(tmp_path)
    probe = state.record_ota_probe(parse_ota_evidence(valid_probe()), now=100)
    challenge, token = state.issue_ota_challenge(
        component="scanner0", artifact_sha256=probe.sha256,
        artifact_crc32=probe.crc32, mode="same-version-recovery", now=100,
    )
    assert state.consume_challenge(challenge.challenge_id, token, now=101)
    with pytest.raises(CanaryApprovalError, match="consumed"):
        state.consume_challenge(challenge.challenge_id, token, now=102)


@pytest.mark.parametrize("mutation", [
    "writes", "validation", "boot", "wildcard", "stale", "restore",
    "target_mac", "target_boot", "topology", "slot", "crc",
])
def test_ota_challenge_revalidates_every_bound_field(tmp_path: Path, mutation: str):
    state = ota_ready_state(tmp_path)
    payload = valid_probe()
    if mutation == "writes": payload["image_writes_after"] = 8
    if mutation == "validation": payload["complete_image_validated"] = False
    if mutation == "boot": payload["boot_id_after"] = 101
    if mutation == "wildcard": payload["sha256"] = "*"
    if mutation == "stale": payload["captured_at"] = 0
    if mutation == "target_mac": payload["actual_target_mac"] = "AA:00:00:00:00:99"
    if mutation == "target_boot": payload["actual_target_boot_id"] = 999
    if mutation == "topology": payload["actual_topology_generation"] = 8
    if mutation == "slot": payload["component_slot"] = 1
    if mutation == "crc": payload.pop("crc32")
    if mutation == "restore": state.record_flash_failure("scanner0", phase="ota", reason="x")
    with pytest.raises((CanaryApprovalError, CanaryOrderError)):
        evidence = parse_ota_evidence(payload)
        state.record_ota_probe(evidence, now=100)
        state.issue_ota_challenge(
            component="scanner0", artifact_sha256=evidence.sha256,
            artifact_crc32=evidence.crc32, mode="newer-only", now=401 if mutation == "stale" else 101,
        )


def test_ota_apply_command_has_all_canonical_fields_in_order():
    line = build_ota_apply_line(
        component="scanner0", sha256="d" * 64,
        mode="same-version-recovery", target_mac="aa:00:00:00:00:01",
        target_boot_id=100, topology_generation=7,
    )
    assert line == (
        "FOF_BACKEND_OTA_APPLY scanner0 " + "d" * 64 +
        " same-version-recovery AA:00:00:00:00:01 100 7\n"
    )
    with pytest.raises(CanaryApprovalError):
        build_ota_apply_line(
            component="scanner0", sha256="*", mode="newer-only",
            target_mac="AA:00:00:00:00:01", target_boot_id=100,
            topology_generation=7,
        )


def test_toolchain_resolution_pins_platformio_python_core_and_esptool(tmp_path: Path):
    pio = private_file(tmp_path / "bin/pio", b"#!/bin/sh\n")
    pio.chmod(0o700)
    python_exe = private_file(tmp_path / "penv/bin/python", b"python")
    python_exe.chmod(0o700)
    core = tmp_path / "core"
    esptool = private_file(
        core / "packages/tool-esptoolpy/esptool.py", b"# pinned esptool\n"
    )
    calls: list[list[str]] = []

    def runner(command, **_kwargs):
        calls.append(command)
        if command[0] == str(pio.resolve()):
            output = json.dumps({
                "platformio_version": "6.1.18",
                "python_exe": str(python_exe),
                "core_dir": str(core),
            })
        else:
            output = "esptool.py v4.11.0\n"
        return subprocess.CompletedProcess(command, 0, stdout=output)

    receipt = resolve_toolchain(pio, runner=runner)
    assert receipt.pio_path == str(pio.resolve())
    assert receipt.python_exe == str(python_exe.resolve())
    assert receipt.core_dir == str(core.resolve())
    assert receipt.esptool_path == str(esptool.resolve())
    assert calls[1] == [str(python_exe.resolve()), str(esptool.resolve()), "version"]
    commands = build_inventory_commands(receipt, "/dev/cu.usbmodem1101")
    assert all(command[:2] == [receipt.python_exe, receipt.esptool_path]
               for command in commands.values())


def test_live_inventory_is_fail_closed_on_security_size_and_mac():
    installed = installed_identity("scanner0", "AA:00:00:00:00:01")
    valid = {
        "security": (
            "Chip is ESP32-S3 (revision v0.2)\n"
            "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
        ),
        "flash": "Detected flash size: 8MB\n",
        "mac": "MAC: AA:00:00:00:00:01\n",
    }
    live = parse_live_inventory(
        installed, role="scanner0", port=installed.port, outputs=valid
    )
    assert live.mac == installed.mac and live.flash_size == FLASH_SIZE
    for key, replacement in (
        ("security", valid["security"].replace("Secure Boot: Disabled", "Secure Boot: Enabled")),
        ("flash", "Detected flash size: 4MB\n"),
        ("mac", "MAC: AA:00:00:00:00:99\n"),
    ):
        changed = dict(valid, **{key: replacement})
        if key == "mac":
            assert parse_live_inventory(
                installed, role="scanner0", port=installed.port, outputs=changed
            ).mac == "AA:00:00:00:00:99"
        else:
            with pytest.raises(CanaryInventoryError):
                parse_live_inventory(
                    installed, role="scanner0", port=installed.port,
                    outputs=changed,
                )


def test_execute_backup_requires_duplicate_and_slice_proof(tmp_path: Path):
    state = CanaryState(toolchain=toolchain())
    identities = {
        role: installed_identity(role, f"AA:00:00:00:00:0{index}")
        for index, role in enumerate(BOARD_ROLES, start=1)
    }
    for identity in identities.values():
        state.record_installed_evidence(identity)
    for identity in identities.values():
        state.record_inventory(
            identity,
            lite_sensor_confirmed=True,
            no_sd_expansion_confirmed=True,
        )
    table = partition_binary(LEGACY_PARTITIONS["scanner"])
    nvs = b"N" * 0x6000

    def runner(command, **_kwargs):
        if "read_flash" in command:
            offset = int(command[-3], 0)
            size = int(command[-2], 0)
            output = Path(command[-1])
            if offset == 0:
                with output.open("wb") as handle:
                    handle.truncate(FLASH_SIZE)
                with output.open("r+b") as handle:
                    handle.seek(0x8000)
                    handle.write(table)
                    handle.seek(0x9000)
                    handle.write(nvs)
            elif offset == 0x9000:
                output.write_bytes(nvs)
            else:
                assert offset == 0x8000 and size == 0x1000
                output.write_bytes(table)
        return subprocess.CompletedProcess(command, 0, stdout="ok\n")

    backup = execute_backup(
        state,
        role="scanner0",
        kind="original",
        output_dir=tmp_path / "backups",
        receipt=toolchain(),
        runner=runner,
        now=100,
    )
    assert Path(backup.full_path).stat().st_size == FLASH_SIZE
    assert state.boards["scanner0"].backups["original"] == backup
    assert not list(Path(backup.full_path).parent.glob("*.tmp"))


def test_backend_workflow_cannot_upload_sensitive_canary_material():
    workflow = Path(__file__).resolve().parents[3] / ".github/workflows/backend-firmware.yml"
    text = workflow.read_text(encoding="utf-8")
    upload = text[text.index("uses: actions/upload-artifact@v4"):]
    assert ".canary" not in upload
    assert "full.bin" not in upload
    assert "nvs.bin" not in upload
    assert "transcript" not in upload
    assert "backend-firmware/web-flasher" in upload
    assert "backend-firmware/release/backend-release-index.json" in upload
    assert "backend-firmware/test-logs" in upload


def test_ignore_contract_covers_every_sensitive_canary_file():
    root = Path(__file__).resolve().parents[2]
    repository = root.parent
    ignore = (root / ".gitignore").read_text(encoding="utf-8")
    assert ".canary/*" in ignore
    assert "!.canary/.gitkeep" in ignore
    assert "/scanner/sdkconfig.scanner-s3-combo-backend" in ignore
    assert "/uplink/sdkconfig.uplink-s3-backend" in ignore
    marker = root / ".canary/.gitkeep"
    assert marker.is_file() and marker.read_bytes() == b""
    sensitive = [
        "backend-firmware/.canary/state.json",
        "backend-firmware/.canary/backups/original/scanner0-full.bin",
        "backend-firmware/.canary/backups/backend-baseline/uplink-nvs.bin",
        "backend-firmware/.canary/transcripts/inventory.json",
        "backend-firmware/.canary/catalog/catalog.json",
        "backend-firmware/.canary/probes/scanner0.json",
        "backend-firmware/.canary/evidence/ota.json",
        "backend-firmware/.canary/ready/scanner0.ready",
        "backend-firmware/.canary/challenges/receipt.json",
        "backend-firmware/scanner/sdkconfig.scanner-s3-combo-backend",
        "backend-firmware/uplink/sdkconfig.uplink-s3-backend",
    ]
    checked = subprocess.run(
        ["git", "check-ignore", "--no-index", *sensitive],
        cwd=repository,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert checked.returncode == 0, checked.stderr
    assert set(checked.stdout.splitlines()) == set(sensitive)
    marker_check = subprocess.run(
        ["git", "check-ignore", "--no-index", "backend-firmware/.canary/.gitkeep"],
        cwd=repository,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert marker_check.returncode == 1
    tracked = subprocess.run(
        ["git", "ls-files", "backend-firmware/.canary"],
        cwd=repository,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.splitlines()
    assert tracked == ["backend-firmware/.canary/.gitkeep"]
