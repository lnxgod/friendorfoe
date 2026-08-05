from __future__ import annotations

from argparse import Namespace
import hashlib
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools.lite_factory_flasher import cli
from tools.badge_flasher.devices import DeviceError
from tools.badge_flasher.models import FlashEvidence, TopologyAssignment, UsbDevice
from tools.lite_factory_flasher.models import PassedLiteFactoryRecord


RESOURCE = (
    Path(__file__).resolve().parents[1]
    / "lite_factory_flasher"
    / "resources"
    / "lite-factory-flasher-embedded.zip"
)


ASSIGNMENT = TopologyAssignment(
    "AA:BB:CC:DD:EE:01",
    "AA:BB:CC:DD:EE:02",
    "AA:BB:CC:DD:EE:03",
)


@pytest.mark.parametrize(
    "argv",
    (
        ["--yes"],
        ["--yes", "--once"],
        ["--yes", "--confirm-product", "badge_lite"],
    ),
)
def test_unattended_mode_is_rejected_before_bundle_or_hardware_access(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    argv: list[str],
) -> None:
    def forbidden(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("unsafe path reached")

    monkeypatch.setattr(cli, "choose_bundle", forbidden)
    monkeypatch.setattr(cli, "banner", forbidden)

    assert cli.main(argv) == 2
    assert "--yes requires --once --confirm-product badge_lite" in capsys.readouterr().err


def test_operator_confirmation_requires_exact_lite_token(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(cli, "_prompt_operator", lambda _message: "lite")
    with pytest.raises(KeyboardInterrupt):
        cli.prompt_lite_confirmation(plain=True)

    monkeypatch.setattr(cli, "_prompt_operator", lambda _message: "LITE")
    cli.prompt_lite_confirmation(plain=True)


def test_prior_pass_allows_only_exact_same_bundle_and_graph() -> None:
    prior = PassedLiteFactoryRecord(
        version="0.2.0-backend",
        bundle_sha256="a" * 64,
        assignment=ASSIGNMENT,
    )
    connected = {
        ASSIGNMENT.uplink_mac,
        ASSIGNMENT.ble_leaf_mac,
        ASSIGNMENT.wifi_leaf_mac,
    }

    assert cli._prior_pass(
        connected,
        (prior,),
        version=prior.version,
        bundle_sha256=prior.bundle_sha256,
    ) == prior

    with pytest.raises(DeviceError, match="different prior PASS graph"):
        cli._prior_pass(
            connected,
            (prior,),
            version="0.3.0-backend",
            bundle_sha256="b" * 64,
        )


def test_rework_requires_the_same_discovered_role_graph() -> None:
    prior = PassedLiteFactoryRecord(
        version="0.2.0-backend",
        bundle_sha256="a" * 64,
        assignment=ASSIGNMENT,
    )
    cli._require_prior_graph(prior, ASSIGNMENT)

    rewired = TopologyAssignment(
        ASSIGNMENT.uplink_mac,
        ASSIGNMENT.wifi_leaf_mac,
        ASSIGNMENT.ble_leaf_mac,
    )
    with pytest.raises(DeviceError, match="authoritative prior PASS graph"):
        cli._require_prior_graph(prior, rewired)


def test_offline_bundle_choice_uses_the_validated_embedded_release() -> None:
    args = Namespace(bundle=None, offline=True, allow_downgrade=False)

    selected = cli.choose_bundle(args, plain=True)

    assert selected.source == "embedded"
    assert selected.version == "0.2.0-backend"
    assert selected.scanner_version == "0.67.2-badge-defcon34"


def test_release_lookup_is_offline_by_default_and_requires_explicit_online() -> None:
    assert cli.parser().parse_args([]).offline is True
    assert cli.parser().parse_args(["--offline"]).offline is True
    assert cli.parser().parse_args(["--online"]).offline is False


def test_untrusted_local_candidate_requires_its_exact_digest(tmp_path: Path) -> None:
    candidate = tmp_path / "candidate.zip"
    candidate.write_bytes(RESOURCE.read_bytes() + b"candidate-trailer")
    digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
    base = {
        "bundle": candidate,
        "offline": True,
        "allow_downgrade": False,
    }

    with pytest.raises(cli.BundleError, match="--accept-candidate-sha256"):
        cli.choose_bundle(
            Namespace(**base, accept_candidate_sha256=None),
            plain=True,
        )

    selected = cli.choose_bundle(
        Namespace(**base, accept_candidate_sha256=digest),
        plain=True,
    )
    assert selected.bundle_sha256 == digest


def test_explicit_digest_binds_even_for_a_trusted_release() -> None:
    args = Namespace(
        bundle=RESOURCE,
        offline=True,
        allow_downgrade=False,
        accept_candidate_sha256="0" * 64,
    )

    with pytest.raises(cli.BundleError, match="does not match the selected"):
        cli.choose_bundle(args, plain=True)


def test_candidate_digest_flag_is_limited_to_one_local_bundle_run(
    capsys: pytest.CaptureFixture[str],
) -> None:
    digest = "a" * 64
    assert cli.main(["--accept-candidate-sha256", digest]) == 2
    assert "requires --bundle and --once" in capsys.readouterr().err


def test_post_commit_output_failure_never_appends_a_contradictory_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    result = SimpleNamespace(
        assignment=ASSIGNMENT,
        version="0.2.0-backend",
        bundle_sha256="a" * 64,
        receipt="lite_TESTPASS",
    )
    bundle = SimpleNamespace(
        version=result.version,
        scanner_version="0.67.2-badge-defcon34",
        bundle_sha256=result.bundle_sha256,
    )

    class FakeLedger:
        def __init__(self) -> None:
            self.committed = False

        def passed_records(self) -> tuple[PassedLiteFactoryRecord, ...]:
            return ()

        def record(self, observed: object) -> bool:
            assert observed is result
            self.committed = True
            return True

        def record_failure(self, **_kwargs: object) -> bool:
            raise AssertionError("FAIL must not be recorded after PASS commit")

    ledger = FakeLedger()
    monkeypatch.setattr(cli, "choose_bundle", lambda *_args: bundle)
    monkeypatch.setattr(cli, "run_one", lambda *_args, **_kwargs: result)
    monkeypatch.setattr(cli, "_run_factory_operation", lambda operation: operation())
    monkeypatch.setattr(
        cli,
        "print_user_visible",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(BrokenPipeError()),
    )

    status = cli._run_locked_factory(
        Namespace(yes=True, once=True),
        plain=True,
        ledger=ledger,  # type: ignore[arg-type]
    )

    assert ledger.committed is True
    assert status == 0


def test_factory_cycle_uses_exact_probe_and_production_order_without_game_seed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    devices = {
        mac: UsbDevice(
            mac=mac,
            port=f"/dev/fake{index}",
            chip="ESP32-S3",
            revision="v0.2",
            flash_size="8MB",
            psram_size="8MB",
        )
        for index, mac in enumerate((
            ASSIGNMENT.uplink_mac,
            ASSIGNMENT.ble_leaf_mac,
            ASSIGNMENT.wifi_leaf_mac,
        ))
    }
    calls: list[tuple[str, str]] = []
    handoffs: list[str] = []

    class FakeBackend:
        def list_candidate_ports(self) -> list[str]:
            return [device.port for device in devices.values()]

        def scan(self) -> dict[str, UsbDevice]:
            return devices

        def rebind(
            self,
            _macs: set[str],
            *,
            timeout_s: float,
        ) -> dict[str, UsbDevice]:
            assert timeout_s == 30
            return devices

    class FakeEngine:
        def flash_and_verify(
            self,
            device: UsbDevice,
            bundle: object,
            role: str,
        ) -> FlashEvidence:
            calls.append((device.mac, role))
            return FlashEvidence(
                mac=device.mac,
                role=role,
                port=device.port,
                version=(
                    "1.0.0" if role == "probe" else bundle.version
                ),
                write_verified=True,
                readback_verified=True,
            )

        def handoff_to_application(self, device: UsbDevice) -> None:
            handoffs.append(device.mac)

    snapshots = iter((SimpleNamespace(name="first"), SimpleNamespace(name="second")))
    bundle = SimpleNamespace(
        version="0.2.0-backend",
        scanner_version="0.67.2-badge-defcon34",
        bundle_sha256="a" * 64,
    )
    monkeypatch.setattr(cli, "DeviceBackend", FakeBackend)
    monkeypatch.setattr(cli, "FlashEngine", FakeEngine)
    monkeypatch.setattr(cli, "usb_jtag_app_reset", lambda _port: None)
    monkeypatch.setattr(
        cli,
        "rebind_probe_ports",
        lambda _ports, _macs, timeout_s: devices,
    )
    monkeypatch.setattr(
        cli,
        "discover_topology",
        lambda _devices, timeout_s: ASSIGNMENT,
    )
    monkeypatch.setattr(
        cli,
        "wait_for_stable_runtime",
        lambda *_args, **_kwargs: next(snapshots),
    )
    monkeypatch.setattr(cli, "verify_reboot_transition", lambda *_args: None)
    monkeypatch.setattr(
        cli,
        "runtime_evidence",
        lambda first, second: {"first": first.name, "second": second.name},
    )
    monkeypatch.setattr(cli.time, "sleep", lambda _seconds: None)

    result = cli.run_one(
        Namespace(allow_rework=False),
        plain=True,
        bundle=bundle,
    )

    probe_order = sorted(devices)
    assert calls == [
        *((mac, "probe") for mac in probe_order),
        (ASSIGNMENT.ble_leaf_mac, "scanner"),
        (ASSIGNMENT.wifi_leaf_mac, "scanner"),
        (ASSIGNMENT.uplink_mac, "uplink"),
    ]
    assert handoffs == [
        ASSIGNMENT.ble_leaf_mac,
        ASSIGNMENT.wifi_leaf_mac,
        ASSIGNMENT.uplink_mac,
    ] * 2
    assert [item.role for item in result.devices] == [
        "scanner", "scanner", "uplink"
    ]
    assert [item.version for item in result.devices] == [
        bundle.scanner_version,
        bundle.scanner_version,
        bundle.version,
    ]
    assert result.runtime == {"first": "first", "second": "second"}
