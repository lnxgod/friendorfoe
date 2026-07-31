from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _runtime_source() -> str:
    return (
        ROOT
        / "esp32"
        / "uplink"
        / "main"
        / "core"
        / "badge_runtime.c"
    ).read_text()


def test_preinit_usb_arm_is_not_consumed_or_forgotten_by_runtime_init():
    runtime = _runtime_source()

    assert "static bool s_boot_rtc_transition_cached" in runtime
    assert (
        "static badge_runtime_rtc_boot_result_t "
        "s_boot_rtc_transition_result"
    ) in runtime
    helper_start = runtime.index(
        "static badge_runtime_rtc_boot_result_t "
        "boot_rtc_transition_locked("
    )
    helper = runtime[
        helper_start :
        runtime.index("static bool rtc_layout_valid(", helper_start)
    ]
    assert "if (!s_boot_rtc_transition_cached)" in helper
    assert "badge_runtime_rtc_transition(" in helper
    assert "s_boot_rtc_transition_cached = true;" in helper

    arm_signature = runtime.index("badge_runtime_arm_expected_reboot(")
    arm_start = runtime.rindex(
        "badge_runtime_expected_reboot_arm_result_t", 0, arm_signature
    )
    arm = runtime[
        arm_start :
        runtime.index("void badge_runtime_set_expected_reboot_hook(", arm_start)
    ]
    cache_prior_boot = arm.index("boot_rtc_transition_locked()")
    reserve_owner = arm.index(
        "badge_runtime_expected_reboot_arm_reserve(", cache_prior_boot
    )
    publish_owner = arm.index(
        "badge_runtime_expected_reboot_arm_publish(", reserve_owner
    )
    publish_magic = arm.index(
        "&g_fof_badge_rtc_state.expected_reboot_magic", publish_owner
    )
    assert cache_prior_boot < reserve_owner < publish_owner < publish_magic

    init_start = runtime.index("void badge_runtime_init(bool pending_verify)")
    init = runtime[
        init_start :
        runtime.index(
            "void badge_runtime_set_pending_verify(", init_start
        )
    ]
    assert "boot_rtc_transition_locked()" in init
    assert "badge_runtime_rtc_transition(" not in init
    assert "badge_runtime_expected_reboot_arm_state_init(" not in init

    classify_start = runtime.index(
        "bool badge_runtime_reset_reason_was_expected_software("
    )
    classify = runtime[
        classify_start :
        runtime.index(
            "bool badge_runtime_usb_recovery_once_consumed(",
            classify_start,
        )
    ]
    assert "s_boot_rtc_transition_cached" in classify
    assert "s_boot_rtc_transition_result.expected_software_reset" in classify
