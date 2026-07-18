from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _text(relative: str) -> str:
    return (REPO_ROOT / relative).read_text()


def test_theme_reset_reports_persistence_failure_to_every_control_transport():
    header = _text("esp32/uplink/main/core/badge_theme_runtime.h")
    serial = _text("esp32/uplink/main/core/serial_config.c")
    http = _text("esp32/uplink/main/network/http_status.c")
    ble = _text("esp32/uplink/main/core/badge_ble_control.c")

    assert "bool badge_theme_runtime_reset(bool persist);" in header

    serial_reset = serial[
        serial.index("static void handle_badge_theme_reset_command") :
        serial.index("static void handle_ctl_command", serial.index(
            "static void handle_badge_theme_reset_command"
        ))
    ]
    assert "if (!badge_theme_runtime_reset(persist))" in serial_reset
    assert 'send_control_error("badge theme reset failed")' in serial_reset
    assert serial_reset.index("if (!badge_theme_runtime_reset(persist))") < serial_reset.index(
        'FOF_CTL_OK:{\\"message\\":\\"badge theme reset\\"'
    )

    http_reset = http[
        http.index('} else if (strcmp(cmd, "badge_theme_reset") == 0)') :
        http.index('} else if (strcmp(cmd, "display_nav") == 0)', http.index(
            '} else if (strcmp(cmd, "badge_theme_reset") == 0)'
        ))
    ]
    assert "if (!badge_theme_runtime_reset(persist))" in http_reset
    assert '"{\\"ok\\":false,\\"error\\":\\"theme reset failed\\"}"' in http_reset

    ble_reset = ble[
        ble.index('} else if (strcmp(cmd, "badge_theme_reset") == 0)') :
        ble.index('} else if (strcmp(cmd, "ble_investigate") == 0)', ble.index(
            '} else if (strcmp(cmd, "badge_theme_reset") == 0)'
        ))
    ]
    assert "ok = badge_theme_runtime_reset(persist);" in ble_reset
    assert 'badge_ble_set_error(ok ? "" : "badge theme reset failed")' in ble_reset
    assert 'reply = ok ? "badge theme reset" : s_last_error;' in ble_reset
