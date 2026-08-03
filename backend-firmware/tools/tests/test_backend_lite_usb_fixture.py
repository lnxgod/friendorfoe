from __future__ import annotations

import pytest

from tools.backend_lite_usb_fixture import (
    LiveSession,
    ProtocolError,
    build_config_get,
    build_config_set,
    build_live_start,
    build_ping,
    build_status,
    parse_config,
    parse_detection,
    verify_lite_handshake,
)


STATUS_FRAME = (
    'FOF_STATUS:{"product_family":"badge_lite",'
    '"target":"uplink-s3-backend",'
    '"project":"fof_backend_uplink",'
    '"hardware":"seeed_xiao_esp32s3",'
    '"version":"0.2.0-backend",'
    '"mode":"headless",'
    '"capabilities":["display_none","usb_live","usb_live_ack",'
    '"usb_buffered","usb_config","http_uplink","config_ap",'
    '"ap_dashboard","remote_ota","uart_relay_ota"]}'
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


def test_live_ready_parses_current_session_and_timing():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    assert session.session_id == "boot-a1"
    assert session.heartbeat_ms == 5000
    assert session.lease_ms == 15000


def test_ack_echoes_current_heartbeat_only():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    assert session.ack(
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    ) == 'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":7}'


def test_stale_session_and_replayed_heartbeat_are_rejected():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-b2",'
        '"heartbeat_ms":5000,"lease_ms":15000}'
    )

    with pytest.raises(ProtocolError):
        session.ack(
            'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":8}'
        )

    current = (
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-b2","sequence":8}'
    )
    assert session.ack(current) == (
        'FOF_LIVE_ACK:{"session_id":"boot-b2","sequence":8}'
    )
    with pytest.raises(ProtocolError):
        session.ack(current)


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
