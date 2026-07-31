"""Mandatory post-build layout verification for the badge uplink image."""

import os
from pathlib import Path
import sys
import tempfile


Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

scripts_dir = (
    Path(os.path.abspath(env.subst("$PROJECT_DIR"))).parent / "scripts"
)
sys.path.insert(0, str(scripts_dir))

from verify_badge_uplink_build import (  # noqa: E402
    UPLINK_CANARY_RTC_NOINIT_BYTES,
    UPLINK_PRODUCTION_RTC_NOINIT_BYTES,
    prepare_verified_badge_uplink_snapshot,
    verify_frozen_badge_uplink_attestation,
    verify_badge_uplink_canary_sdkconfig,
)


def verify_after_badge_build(source, target, env):
    del source, target
    build_dir = Path(env.subst("$BUILD_DIR"))
    project_dir = Path(env.subst("$PROJECT_DIR"))
    pio_env = env.subst("$PIOENV")
    sdkconfig = project_dir / f"sdkconfig.{pio_env}"
    is_canary = pio_env == "uplink-s3-fof_badge-con-crud-canary"
    if is_canary:
        config_errors = verify_badge_uplink_canary_sdkconfig(sdkconfig)
        if config_errors:
            raise RuntimeError(
                "unsafe CON CRUD canary Bluetooth configuration: "
                + "; ".join(config_errors)
            )
    rtc_expected_size = (
        UPLINK_CANARY_RTC_NOINIT_BYTES
        if is_canary
        else UPLINK_PRODUCTION_RTC_NOINIT_BYTES
    )
    with tempfile.TemporaryDirectory(
        prefix="fof-uplink-artifact-snapshot-",
        dir=project_dir / ".pio",
    ) as private_parent:
        snapshot = prepare_verified_badge_uplink_snapshot(
            build_dir,
            project_dir / "partitions_s3_fof_badge_8mb.csv",
            sdkconfig,
            private_parent=Path(private_parent),
            materialize_missing_aliases=True,
        )
        try:
            frozen = snapshot.freeze_for_mutation()
        finally:
            snapshot.close()
    rtc_errors = verify_frozen_badge_uplink_attestation(
        frozen,
        label=(
            "canary badge uplink"
            if is_canary
            else "production badge uplink"
        ),
        expected_rtc_size=rtc_expected_size,
    )
    if rtc_errors:
        raise RuntimeError(
            "unsafe badge uplink RTC ABI: " + "; ".join(rtc_errors)
        )
    print(
        "FoF: badge uplink immutable artifact snapshot verified "
        f"{frozen.aggregate_sha256}"
    )


if env.subst("$PIOENV") in {
    "uplink-s3-fof_badge",
    "uplink-s3-fof_badge-con-crud-canary",
}:
    buildprog = env.Alias("buildprog")
    env.AlwaysBuild(buildprog)
    env.AddPostAction(buildprog, verify_after_badge_build)
