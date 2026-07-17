"""PlatformIO pre-build hook that refreshes stale ESP-IDF CMake metadata."""

from pathlib import Path
import os
import sys


Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

script_dir = Path(env.subst("$PROJECT_DIR")).resolve().parent / "scripts"
sys.path.insert(0, str(script_dir))

from firmware_version import expected_version_for_env, invalidate_stale_cmake_cache


project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
environment = env.subst("$PIOENV")
version_header = project_dir.parent / "shared" / "version.h"
expected_version = expected_version_for_env(version_header, environment)

# ESP-IDF's CMake subprocess does not inherit PlatformIO's PIOENV construction
# variable automatically. Export it so each environment selects its own track.
os.environ["PIOENV"] = environment
env["ENV"]["PIOENV"] = environment

if invalidate_stale_cmake_cache(build_dir, expected_version):
    print(
        "FoF: invalidated stale CMake PROJECT_VER for "
        f"{environment}; expected {expected_version}"
    )
