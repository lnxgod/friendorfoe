import unittest

from new_dash.controls import (
    BadgeControlCommand,
    ControlValidationError,
    build_display_nav,
    build_display_policy,
    build_display_policy_reset,
    build_theme,
    build_theme_reset,
)


THEME = {
    "version": 1,
    "palette": "night",
    "background": "dark",
    "brightness": 80,
    "accents": {
        "drone": 65184,
        "meta": 63539,
        "tracker": 63519,
        "flock": 43039,
        "wifi_attack": 2047,
        "clear": 12133,
    },
}

POLICY_CLASSES = (
    "drone", "meta", "tracker", "wifi_attack", "skimmer", "camera",
    "flock", "lock", "hid", "beacon", "event_badge", "auracast",
    "scanner_status",
)


def complete_policy() -> dict[str, object]:
    return {
        "version": 1,
        "classes": {
            class_name: {
                "enabled": True,
                "lane": "lower",
                "min_proximity": "near",
                "priority": 50,
            }
            for class_name in POLICY_CLASSES
        },
    }


class ControlValidationTest(unittest.TestCase):
    def test_navigation_is_canonical_and_transient(self) -> None:
        command = build_display_nav("detail")
        self.assertEqual(command.payload, {"cmd": "display_nav", "action": "detail"})
        self.assertEqual(command.expected_message, "display nav updated")
        self.assertEqual(command.to_wire(), b'FOF_CTL:{"cmd":"display_nav","action":"detail"}\n')
        for rejected in ("select", "close", "reboot", "bootloader", "", True):
            with self.subTest(rejected=rejected), self.assertRaises(ControlValidationError):
                build_display_nav(rejected)  # type: ignore[arg-type]

    def test_theme_inserts_persistence_and_normalizes_nested_values(self) -> None:
        theme = dict(THEME, accents=dict(THEME["accents"]))
        command = build_theme(theme)
        self.assertEqual(command.payload, {"cmd": "badge_theme", "persist": True, "theme": theme})
        self.assertEqual(command.expected_message, "badge theme updated")
        theme["accents"]["drone"] = 1
        self.assertEqual(command.payload["theme"]["accents"]["drone"], 65184)  # type: ignore[index]

    def test_theme_rejects_missing_unknown_and_non_integer_fields(self) -> None:
        cases = []
        missing = dict(THEME)
        missing.pop("brightness")
        cases.append(missing)
        unknown = dict(THEME, extra="rejected")
        cases.append(unknown)
        bad_version = dict(THEME, version=True)
        cases.append(bad_version)
        bad_brightness = dict(THEME, brightness=24)
        cases.append(bad_brightness)
        bad_accent = dict(THEME, accents=dict(THEME["accents"], drone=True))
        cases.append(bad_accent)
        missing_accent = dict(THEME, accents={"drone": 1})
        cases.append(missing_accent)
        for payload in cases:
            with self.subTest(payload=payload), self.assertRaises(ControlValidationError):
                build_theme(payload)

    def test_policy_requires_complete_schema_and_normalizes_nested_values(self) -> None:
        policy = complete_policy()
        command = build_display_policy(policy)
        self.assertEqual(
            command.payload,
            {"cmd": "badge_display_policy", "persist": True, "policy": policy},
        )
        self.assertEqual(command.expected_message, "display policy updated")
        policy["classes"]["drone"]["priority"] = 99  # type: ignore[index]
        self.assertEqual(command.payload["policy"]["classes"]["drone"]["priority"], 50)  # type: ignore[index]

    def test_policy_rejects_missing_unknown_and_invalid_class_values(self) -> None:
        cases = []
        missing_class = complete_policy()
        missing_class["classes"].pop("drone")  # type: ignore[union-attr]
        cases.append(missing_class)
        unknown_class = complete_policy()
        unknown_class["classes"]["firmware"] = {}  # type: ignore[index]
        cases.append(unknown_class)
        missing_field = complete_policy()
        missing_field["classes"]["drone"].pop("priority")  # type: ignore[index]
        cases.append(missing_field)
        unknown_field = complete_policy()
        unknown_field["classes"]["drone"]["extra"] = "no"  # type: ignore[index]
        cases.append(unknown_field)
        bad_enabled = complete_policy()
        bad_enabled["classes"]["drone"]["enabled"] = 1  # type: ignore[index]
        cases.append(bad_enabled)
        bad_lane = complete_policy()
        bad_lane["classes"]["drone"]["lane"] = "off"  # type: ignore[index]
        cases.append(bad_lane)
        disabled_lane = complete_policy()
        disabled_lane["classes"]["drone"].update(enabled=False, lane="lower")  # type: ignore[index]
        cases.append(disabled_lane)
        bad_priority = complete_policy()
        bad_priority["classes"]["drone"]["priority"] = True  # type: ignore[index]
        cases.append(bad_priority)
        for payload in cases:
            with self.subTest(payload=payload), self.assertRaises(ControlValidationError):
                build_display_policy(payload)

    def test_reset_commands_are_persistent_and_have_exact_messages(self) -> None:
        self.assertEqual(
            (build_theme_reset().payload, build_theme_reset().expected_message),
            ({"cmd": "badge_theme_reset", "persist": True}, "badge theme reset"),
        )
        self.assertEqual(
            (build_display_policy_reset().payload, build_display_policy_reset().expected_message),
            ({"cmd": "badge_display_policy_reset", "persist": True}, "display policy reset"),
        )


class ControlWireTest(unittest.TestCase):
    def test_wire_uses_ascii_compact_finite_json(self) -> None:
        command = BadgeControlCommand({"message": "caf\u00e9"}, "unused")
        self.assertEqual(command.to_wire(), b'FOF_CTL:{"message":"caf\\u00e9"}\n')
        with self.assertRaises(ControlValidationError):
            BadgeControlCommand({"value": float("nan")}, "unused").to_wire()

    def test_wire_accepts_2047_bytes_before_newline_and_rejects_2048(self) -> None:
        prefix_size = len(BadgeControlCommand({"x": ""}, "unused").to_wire()) - 1
        accepted = BadgeControlCommand({"x": "a" * (2047 - prefix_size)}, "unused")
        rejected = BadgeControlCommand({"x": "a" * (2048 - prefix_size)}, "unused")
        self.assertEqual(len(accepted.to_wire()) - 1, 2047)
        with self.assertRaises(ControlValidationError):
            rejected.to_wire()
