from __future__ import annotations

from pathlib import Path

import pytest

from tools.backend_lite_usb_fixture import (
    ConfigTransaction,
    LiveHandshake,
    LiveSession,
    ProtocolError,
    STATUS_MAX_BYTES,
    VerifiedLiteDevice,
    build_config_get,
    build_config_set,
    build_live_start,
    build_ping,
    build_status,
    parse_config,
    parse_detection,
    parse_status,
    verify_lite_handshake,
)


STATUS_FRAME = (
    'FOF_STATUS:{"product_family":"badge_lite",'
    '"target":"uplink-s3-backend",'
    '"project":"fof_backend_uplink",'
    '"hardware":"seeed_xiao_esp32s3",'
    '"version":"0.2.0-backend",'
    '"mac":"AA:BB:CC:DD:EE:FF",'
    '"boot_id":305419896,'
    '"mode":"headless",'
    '"mode_label":"Backend Badge Lite",'
    '"config_generation":9,'
    '"capabilities":["display_none","usb_live","usb_live_ack",'
    '"usb_buffered","usb_config","http_uplink","config_ap",'
    '"ap_dashboard","remote_ota","uart_relay_ota"],'
    '"wifi":{"configured":false,"connected":false,'
    '"full_pass_failed":false},'
    '"recovery":{"reason":"wifi_unconfigured","ap_running":true},'
    '"scanner":[{"slot":0,"connected":true,"identity_valid":true},'
    '{"slot":1,"connected":false,"identity_valid":false}],'
    '"threats":{"drone_active":false,"meta_active":false,'
    '"drone_count":0,"meta_count":0,'
    '"drone_last_seen_age_ms":-1,"meta_last_seen_age_ms":-1},'
    '"led":"network_degraded","ota_ready":true,'
    '"upload":{"depth":0,"capacity":512,"dropped":0,"ok":0,'
    '"failed":0,"retries":0},'
    '"usb":{"available":true,"host_connected":true,'
    '"required_depth":0,"optional_depth":0,"optional_drops":0,'
    '"required_failures":0,"bytes_transmitted":0,"bytes_received":0,'
    '"output_poisoned":false},'
    '"live":{"started":false,"session_id":"",'
    '"last_ack_sequence":0,"confirmed":false,"lease_remaining_ms":0},'
    '"history":{"available":true,"count":0,"contention_drops":0},'
    '"dashboard":{"enabled":true,"degraded_reason":null},'
    '"backend":{"reachable":false,"last_success_age_s":null},'
    '"scanner_summaries":[{"slot":0,"connected":true,'
    '"identity_valid":true,"status_available":true,'
    '"identity":{"target":"scanner-s3-combo-backend",'
    '"project":"fof_backend_scanner",'
    '"hardware":"seeed_xiao_esp32s3",'
    '"version":"0.2.0-backend"},"profile":1,'
    '"health":{"command":true,"radio":true,"role_acked":true},'
    '"errors":{"rx":0,"tx_drops":0},"uptime_ms":9000},'
    '{"slot":1,"connected":false,"identity_valid":false,'
    '"status_available":false,"identity":null,"profile":null,'
    '"health":{"command":false,"radio":false,"role_acked":false},'
    '"errors":null,"uptime_ms":null}]}'
)


def verified_device():
    return verify_lite_handshake("FOF_PONG:0.2.0-backend", STATUS_FRAME)


def test_read_only_commands_and_live_start_have_exact_transcripts():
    assert build_ping() == "FOF_PING"
    assert build_status() == "FOF_STATUS"
    assert build_config_get() == "FOF_CONFIG_GET"
    assert build_live_start() == (
        'FOF_LIVE_START:{"client":"new_dash","protocol":1}'
    )


def test_truthful_pong_and_status_enable_only_headless_lite_mutation():
    device = verified_device()

    assert device.identity == (
        "badge_lite",
        "uplink-s3-backend",
        "fof_backend_uplink",
        "seeed_xiao_esp32s3",
    )
    assert device.version == "0.2.0-backend"
    assert device.mutation_enabled is True
    assert device.screen_supported is False


def test_literal_status_matches_final_encoder_contract():
    status = parse_status(STATUS_FRAME)

    assert len(STATUS_FRAME.encode("utf-8")) <= STATUS_MAX_BYTES
    assert list(status) == [
        "product_family",
        "target",
        "project",
        "hardware",
        "version",
        "mac",
        "boot_id",
        "mode",
        "mode_label",
        "config_generation",
        "capabilities",
        "wifi",
        "recovery",
        "scanner",
        "threats",
        "led",
        "ota_ready",
        "upload",
        "usb",
        "live",
        "history",
        "dashboard",
        "backend",
        "scanner_summaries",
    ]
    assert status["upload"] == {
        "depth": 0,
        "capacity": 512,
        "dropped": 0,
        "ok": 0,
        "failed": 0,
        "retries": 0,
    }
    assert status["led"] == "network_degraded"
    assert status["threats"] == {
        "drone_active": False,
        "meta_active": False,
        "drone_count": 0,
        "meta_count": 0,
        "drone_last_seen_age_ms": -1,
        "meta_last_seen_age_ms": -1,
    }
    assert status["backend"] == {
        "reachable": False,
        "last_success_age_s": None,
    }
    assert status["scanner_summaries"] == [
        {
            "slot": 0,
            "connected": True,
            "identity_valid": True,
            "status_available": True,
            "identity": {
                "target": "scanner-s3-combo-backend",
                "project": "fof_backend_scanner",
                "hardware": "seeed_xiao_esp32s3",
                "version": "0.2.0-backend",
            },
            "profile": 1,
            "health": {"command": True, "radio": True, "role_acked": True},
            "errors": {"rx": 0, "tx_drops": 0},
            "uptime_ms": 9000,
        },
        {
            "slot": 1,
            "connected": False,
            "identity_valid": False,
            "status_available": False,
            "identity": None,
            "profile": None,
            "health": {"command": False, "radio": False, "role_acked": False},
            "errors": None,
            "uptime_ms": None,
        },
    ]


def test_handoff_document_uses_the_same_literal_status_frame():
    handoff = (
        Path(__file__).resolve().parents[3]
        / "docs"
        / "backend-lite-new-dash-usb-protocol.md"
    ).read_text(encoding="utf-8")
    documented = next(
        line[2:] for line in handoff.splitlines() if line.startswith("< FOF_STATUS:")
    )

    assert documented == STATUS_FRAME


@pytest.mark.parametrize(
    ("pong", "status"),
    [
        (
            "FOF_PONG:0.2.0-backend",
            STATUS_FRAME.replace("badge_lite", "badge"),
        ),
        (
            "FOF_PONG:0.2.0-backend",
            STATUS_FRAME.replace("uplink-s3-backend", "uplink-s3-fof_badge"),
        ),
        (
            "FOF_PONG:0.2.0-backend",
            STATUS_FRAME.replace("fof_backend_uplink", "fof_badge_uplink"),
        ),
        (
            "FOF_PONG:0.2.0-backend",
            STATUS_FRAME.replace("seeed_xiao_esp32s3", "unexpected_board"),
        ),
        ("FOF_PONG:0.1.9-backend", STATUS_FRAME),
    ],
)
def test_identity_or_version_mismatch_never_enables_mutation(pong, status):
    with pytest.raises(ProtocolError):
        verify_lite_handshake(pong, status)


def test_verified_device_cannot_be_constructed_without_handshake_proof():
    with pytest.raises(ProtocolError):
        VerifiedLiteDevice(
            version="0.2.0-backend",
            capabilities=verified_device().capabilities,
        )


def test_forged_verified_device_cannot_build_mutation():
    legitimate = verified_device()
    forged = object.__new__(VerifiedLiteDevice)
    object.__setattr__(forged, "version", legitimate.version)
    object.__setattr__(forged, "capabilities", legitimate.capabilities)
    object.__setattr__(forged, "_proof", object())

    with pytest.raises(ProtocolError):
        build_config_set(forged, {"display_name": "Forged"})


def test_live_ready_parses_current_session_and_timing():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    assert session.session_id == "boot-a1"
    assert session.heartbeat_ms == 5000
    assert session.lease_ms == 15000


def test_start_retry_rejects_ready_from_previous_generation():
    handshake = LiveHandshake()
    first = handshake.start()
    second = handshake.start()

    assert first.frame == (
        'FOF_LIVE_START:{"client":"new_dash","protocol":1}'
    )
    assert second.frame == first.frame
    assert second.generation == first.generation + 1
    with pytest.raises(ProtocolError):
        handshake.accept_ready(
            first,
            'FOF_LIVE_READY:{"session_id":"stale",'
            '"heartbeat_ms":5000,"lease_ms":15000}',
        )

    session = handshake.accept_ready(
        second,
        'FOF_LIVE_READY:{"session_id":"current",'
        '"heartbeat_ms":5000,"lease_ms":15000}',
    )
    assert session.session_id == "current"


def test_ack_echoes_current_heartbeat_only():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    heartbeat = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    )
    session.observe_heartbeat(heartbeat, sent_at_ms=100)

    assert session.ack(heartbeat, now_ms=101) == (
        'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":7}'
    )


def test_stale_session_and_replayed_heartbeat_are_rejected():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-b2",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    with pytest.raises(ProtocolError):
        session.observe_heartbeat(
            'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":8}',
            sent_at_ms=100,
        )

    current = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-b2","sequence":8}'
    )
    session.observe_heartbeat(current, sent_at_ms=100)
    assert session.ack(current, now_ms=101) == (
        'FOF_LIVE_ACK:{"session_id":"boot-b2","sequence":8}'
    )
    with pytest.raises(ProtocolError):
        session.ack(current, now_ms=102)


def test_ack_at_exact_heartbeat_freshness_boundary_is_rejected():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )
    heartbeat = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    )
    session.observe_heartbeat(heartbeat, sent_at_ms=100)

    with pytest.raises(ProtocolError):
        session.ack(heartbeat, now_ms=15100)
    assert session.is_confirmed(now_ms=15100) is False


def test_saturated_heartbeat_deadline_fails_open_at_int64_max():
    heartbeat = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    )
    just_before = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )
    just_before.observe_heartbeat(heartbeat, sent_at_ms=2**63 - 101)
    assert just_before.ack(heartbeat, now_ms=2**63 - 2) == (
        'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":7}'
    )

    at_boundary = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )
    at_boundary.observe_heartbeat(heartbeat, sent_at_ms=2**63 - 101)
    with pytest.raises(ProtocolError):
        at_boundary.ack(heartbeat, now_ms=2**63 - 1)


def test_only_latest_physically_sent_heartbeat_can_be_acknowledged():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )
    first = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    )
    latest = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":8}'
    )
    session.observe_heartbeat(first, sent_at_ms=0)
    session.observe_heartbeat(latest, sent_at_ms=5000)

    with pytest.raises(ProtocolError):
        session.ack(first, now_ms=5001)
    assert session.ack(latest, now_ms=5001) == (
        'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":8}'
    )


def test_fresh_ack_confirms_renews_and_expiry_reopens_eligible_ap():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )
    assert session.recovery_ap_running(
        now_ms=0,
        wifi_configured=False,
        wifi_connected=False,
        wifi_join_failed=False,
    ) is True

    first = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":1}'
    )
    session.observe_heartbeat(first, sent_at_ms=0)
    session.ack(first, now_ms=1)
    assert session.recovery_ap_running(
        now_ms=1,
        wifi_configured=False,
        wifi_connected=False,
        wifi_join_failed=False,
    ) is False

    second = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":2}'
    )
    session.observe_heartbeat(second, sent_at_ms=5000)
    session.ack(second, now_ms=6000)
    assert session.is_confirmed(now_ms=20999) is True
    assert session.recovery_ap_running(
        now_ms=21000,
        wifi_configured=False,
        wifi_connected=False,
        wifi_join_failed=False,
    ) is True


def test_detection_parser_accepts_the_nine_compatibility_fields():
    detection = parse_detection(
        'FOF_DET:{"id":"rid-1","manufacturer":"DJI",'
        '"badge_label":"Drone","badge_class":"drone",'
        '"badge_entity_key":"drone:rid-1","source":2,'
        '"confidence":0.91,"threat_score":91,"rssi":-54}'
    )

    assert detection.id == "rid-1"
    assert detection.manufacturer == "DJI"
    assert detection.badge_label == "Drone"
    assert detection.badge_class == "drone"
    assert detection.badge_entity_key == "drone:rid-1"
    assert detection.source == 2
    assert detection.confidence == pytest.approx(0.91)
    assert detection.threat_score == 91
    assert detection.rssi == -54


def test_config_set_is_compact_and_requires_verified_lite_identity():
    update = {
        "networks": [{"ssid": "Lab", "password": "lab-secret"}],
        "backend_url": "http://10.0.0.2:8000",
        "display_name": "Lite Lab",
    }

    assert build_config_set(verified_device(), update) == (
        'FOF_CONFIG_SET:{"networks":[{"ssid":"Lab",'
        '"password":"lab-secret"}],'
        '"backend_url":"http://10.0.0.2:8000",'
        '"display_name":"Lite Lab"}'
    )
    with pytest.raises(ProtocolError):
        build_config_set(None, update)


def test_config_set_allows_lite_to_clear_all_saved_networks():
    assert build_config_set(verified_device(), {"networks": []}) == (
        'FOF_CONFIG_SET:{"networks":[]}'
    )


def test_config_get_parser_exposes_presence_flags_but_no_password_values():
    frame = (
        'FOF_CONFIG:{"schema_version":1,"generation":9,'
        '"networks":[{"ssid":"Lab","password_set":true}],'
        '"backend_url":"http://10.0.0.2:8000",'
        '"device_id":"uplink_CB77A4","display_name":"Lite Lab",'
        '"ap_password_set":true,"auto_update_enabled":false,'
        '"has_location":false,"latitude":null,"longitude":null,'
        '"altitude_m":null}'
    )

    config = parse_config(frame)

    assert config.networks[0].ssid == "Lab"
    assert config.networks[0].password_set is True
    assert config.ap_password_set is True
    assert "lab-secret" not in repr(config)
    assert "portal-secret" not in repr(config)


def test_config_ok_advances_exactly_one_generation():
    transaction = ConfigTransaction(generation=9)

    result = transaction.apply_response(
        'FOF_CONFIG_OK:{"generation":10,"reconnect":true}'
    )

    assert result.committed is True
    assert result.reconnect is True
    assert result.reason is None
    assert result.generation == 10
    assert transaction.generation == 10


def test_config_error_preserves_generation_and_rolls_back_candidate():
    transaction = ConfigTransaction(generation=9)

    result = transaction.apply_response(
        'FOF_CONFIG_ERROR:{"reason":"invalid_config"}'
    )

    assert result.committed is False
    assert result.reconnect is False
    assert result.reason == "invalid_config"
    assert result.generation == 9
    assert transaction.generation == 9


@pytest.mark.parametrize(
    "forbidden_member",
    [
        '"password":"lab-secret"',
        '"ap_password":"portal-secret"',
        '"wifi_pass":"lab-secret"',
    ],
)
def test_config_get_rejects_any_raw_password_member(forbidden_member):
    frame = (
        'FOF_CONFIG:{"schema_version":1,"generation":9,'
        '"networks":[{"ssid":"Lab","password_set":true}],'
        '"backend_url":"http://10.0.0.2:8000",'
        '"device_id":"uplink_CB77A4","display_name":"Lite Lab",'
        '"ap_password_set":true,"auto_update_enabled":false,'
        '"has_location":false,"latitude":null,"longitude":null,'
        '"altitude_m":null,'
        f'{forbidden_member}'
        '}'
    )

    with pytest.raises(ProtocolError):
        parse_config(frame)
