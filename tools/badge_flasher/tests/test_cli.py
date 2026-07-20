from __future__ import annotations

import inspect
import unittest

from tools.badge_flasher import cli


class CliSequenceTests(unittest.TestCase):
    def test_main_selects_bundle_once_outside_repeat_loop(self) -> None:
        source = inspect.getsource(cli.main)
        self.assertLess(source.index("bundle = choose_bundle"), source.index("while True"))
        self.assertEqual(source.count("bundle = choose_bundle"), 1)
        self.assertIn("result = run_one(", source)

    def test_repeat_loop_tracks_previous_badge_macs(self) -> None:
        source = inspect.getsource(cli.main)
        self.assertIn("last_badge_macs", source)
        run_source = inspect.getsource(cli.run_one)
        self.assertIn("previous badge is still connected", run_source)

    def test_explicit_bundle_is_never_superseded_by_github(self) -> None:
        source = inspect.getsource(cli.choose_bundle)
        override = source.index("if args.bundle:")
        github = source.index("fetch_github_bundles")
        self.assertLess(override, github)

    def test_post_rebind_resets_scanners_before_uplink(self) -> None:
        source = inspect.getsource(cli.run_one)
        ble = source.index("usb_jtag_app_reset(rebound[assignment.ble_leaf_mac].port)")
        wifi = source.index("usb_jtag_app_reset(rebound[assignment.wifi_leaf_mac].port)")
        uplink = source.index("wait_for_runtime(")
        self.assertLess(ble, uplink)
        self.assertLess(wifi, uplink)


if __name__ == "__main__":
    unittest.main()
