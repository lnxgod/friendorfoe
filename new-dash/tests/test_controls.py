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

    def test_direct_construction_rejects_prohibited_control_payloads(self) -> None:
        with self.assertRaises(ControlValidationError):
            BadgeControlCommand(
                {"cmd": "reboot", "persist": True},
                "rebooted",
            )

    def test_direct_construction_requires_exact_payload_message_pairing(self) -> None:
        with self.assertRaises(ControlValidationError):
            BadgeControlCommand(
                {"cmd": "display_nav", "action": "next"},
                "badge theme updated",
            )

    def test_builder_result_rejects_top_level_mutation(self) -> None:
        command = build_display_nav("next")
        with self.assertRaises(TypeError):
            command.payload["cmd"] = "reboot"
        self.assertEqual(command.to_wire(), b'FOF_CTL:{"cmd":"display_nav","action":"next"}\n')

    def test_builder_result_rejects_nested_mutation(self) -> None:
        command = build_theme(THEME)
        with self.assertRaises(TypeError):
            command.payload["theme"]["accents"]["drone"] = 0  # type: ignore[index]
        self.assertEqual(command.payload["theme"]["accents"]["drone"], 65184)  # type: ignore[index]


class ControlWireTest(unittest.TestCase):
    def test_each_approved_builder_produces_its_exact_wire_and_message(self) -> None:
        policy = complete_policy()
        commands = (
            (
                build_display_nav("next"),
                b'FOF_CTL:{"cmd":"display_nav","action":"next"}\n',
                "display nav updated",
            ),
            (
                build_theme(THEME),
                b'FOF_CTL:{"cmd":"badge_theme","persist":true,"theme":{"version":1,"palette":"night","background":"dark","brightness":80,"accents":{"drone":65184,"meta":63539,"tracker":63519,"flock":43039,"wifi_attack":2047,"clear":12133}}}\n',
                "badge theme updated",
            ),
            (
                build_theme_reset(),
                b'FOF_CTL:{"cmd":"badge_theme_reset","persist":true}\n',
                "badge theme reset",
            ),
            (
                build_display_policy(policy),
                b'FOF_CTL:{"cmd":"badge_display_policy","persist":true,"policy":{"version":1,"classes":{"drone":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"meta":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"tracker":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"wifi_attack":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"skimmer":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"camera":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"flock":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"lock":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"hid":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"beacon":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"event_badge":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"auracast":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50},"scanner_status":{"enabled":true,"lane":"lower","min_proximity":"near","priority":50}}}}\n',
                "display policy updated",
            ),
            (
                build_display_policy_reset(),
                b'FOF_CTL:{"cmd":"badge_display_policy_reset","persist":true}\n',
                "display policy reset",
            ),
        )
        for command, wire, expected_message in commands:
            with self.subTest(expected_message=expected_message):
                self.assertEqual(command.to_wire(), wire)
                self.assertEqual(command.expected_message, expected_message)
                self.assertLessEqual(len(command.to_wire()) - 1, 2047)
