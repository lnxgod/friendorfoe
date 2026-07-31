"""Mandatory post-build layout verification for the badge scanner image."""

import os
from pathlib import Path
import sys
import tempfile


Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

scripts_dir = (
    Path(os.path.abspath(env.subst("$PROJECT_DIR"))).parent / "scripts"
)
sys.path.insert(0, str(scripts_dir))

from verify_badge_scanner_build import (  # noqa: E402
    prepare_verified_badge_scanner_snapshot,
)


def verify_after_badge_build(source, target, env):
    del source, target
    build_dir = Path(env.subst("$BUILD_DIR"))
    project_dir = Path(env.subst("$PROJECT_DIR"))
    with tempfile.TemporaryDirectory(
        prefix="fof-scanner-artifact-snapshot-",
        dir=project_dir / ".pio",
    ) as private_parent:
        snapshot = prepare_verified_badge_scanner_snapshot(
            build_dir,
            project_dir / "partitions_s3_scanner_8mb.csv",
            project_dir / f"sdkconfig.{env.subst('$PIOENV')}",
            private_parent=Path(private_parent),
            materialize_missing_aliases=True,
        )
        try:
            frozen = snapshot.freeze_for_mutation()
        finally:
            snapshot.close()
    print(
        "FoF: badge scanner immutable artifact snapshot verified "
        f"{frozen.aggregate_sha256}"
    )


if env.subst("$PIOENV") in {
    "scanner-s3-combo-fof_badge",
    "scanner-s3-combo-fof_badge-con-crud-canary",
}:
    buildprog = env.Alias("buildprog")
    env.AlwaysBuild(buildprog)
    env.AddPostAction(buildprog, verify_after_badge_build)
