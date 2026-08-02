"""PlatformIO post-build identity gate for backend firmware images."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile


try:
    Import("env")  # type: ignore[name-defined]  # noqa: F821
except NameError:
    _PIO_ENV = None
else:
    _PIO_ENV = env  # type: ignore[name-defined]  # noqa: F821

if "__file__" in globals():
    _TOOL_DIRECTORY = Path(globals()["__file__"]).resolve().parent
elif _PIO_ENV is not None:
    _TOOL_DIRECTORY = Path(_PIO_ENV.subst("$PROJECT_DIR")).resolve().parent / "tools"
else:
    raise RuntimeError("cannot locate backend-firmware tools directory")

try:
    from tools.firmware_identity import FirmwareIdentityError, verify_backend_image
    from tools.verify_backend_build import (
        APP_PARTITION_CAPACITY,
        BACKEND_RELEASES,
        BuildVerificationError,
    )
except ModuleNotFoundError as exc:  # PlatformIO executes this file by path.
    if exc.name is None or not exc.name.startswith("tools"):
        raise
    tool_directory = str(_TOOL_DIRECTORY)
    if tool_directory not in sys.path:
        sys.path.insert(0, tool_directory)
    from firmware_identity import FirmwareIdentityError, verify_backend_image
    from verify_backend_build import (
        APP_PARTITION_CAPACITY,
        BACKEND_RELEASES,
        BuildVerificationError,
    )


def _backend_version() -> str:
    header = _TOOL_DIRECTORY.parent / "shared" / "backend_version.h"
    try:
        text = header.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildVerificationError("cannot read backend version header") from exc
    matches = re.findall(
        r'^#define\s+FOF_VERSION_BACKEND\s+"([^"]+)"\s*$', text, re.MULTILINE
    )
    if len(matches) != 1:
        raise BuildVerificationError("backend version header is not exact")
    return matches[0]


def _atomic_alias(alias: Path, payload: bytes) -> None:
    temporary: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(
            prefix=f".{alias.name}.", suffix=".tmp", dir=alias.parent
        )
        temporary = Path(name)
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, alias)
        temporary = None
    except OSError as exc:
        raise BuildVerificationError(
            f"cannot materialize verified project app alias: {alias}"
        ) from exc
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def verify_post_build_image(
    *,
    build_dir: Path,
    environment: str,
    project_dir: Path,
    version: str,
):
    """Verify PlatformIO's app output and retain its ESP-IDF project alias."""

    releases = {
        release.environment: release for release in BACKEND_RELEASES.values()
    }
    spec = releases.get(environment)
    if spec is None:
        raise BuildVerificationError(
            f"post-build environment is not an exact backend target: {environment!r}"
        )
    build = Path(build_dir)
    project = Path(project_dir)
    if not build.is_dir() or build.is_symlink():
        raise BuildVerificationError("post-build directory is missing")
    if build.name != environment or build.parent.name != "build":
        raise BuildVerificationError("post-build directory does not match environment")
    if not project.is_dir() or project.is_symlink():
        raise BuildVerificationError("post-build project directory is missing")
    if project.resolve() != build.parents[2].resolve():
        raise BuildVerificationError("post-build project and build directory disagree")
    if project.name != spec.kind or project.parent.name != "backend-firmware":
        raise BuildVerificationError("post-build project is outside backend-firmware")

    firmware = build / "firmware.bin"
    if not firmware.is_file() or firmware.is_symlink():
        raise BuildVerificationError("post-build firmware.bin is missing")
    try:
        verified = verify_backend_image(
            firmware,
            target=spec.target,
            project=spec.project,
            hardware=spec.hardware,
            version=version,
            partition_capacity=APP_PARTITION_CAPACITY,
        )
    except FirmwareIdentityError as exc:
        raise BuildVerificationError(f"post-build identity check failed: {exc}") from exc
    try:
        payload = firmware.read_bytes()
    except OSError as exc:
        raise BuildVerificationError("cannot reread verified firmware.bin") from exc
    if hashlib.sha256(payload).hexdigest() != verified.sha256:
        raise BuildVerificationError("firmware.bin changed during post-build verification")

    alias = build / f"{spec.project}.bin"
    if alias.is_symlink():
        raise BuildVerificationError("project app alias must not be a symlink")
    if not alias.is_file() or alias.read_bytes() != payload:
        _atomic_alias(alias, payload)
    return verified


def _platformio_post_action(source, target, env) -> None:
    del source, target
    verify_post_build_image(
        build_dir=Path(env.subst("$BUILD_DIR")),
        environment=env.subst("$PIOENV"),
        project_dir=Path(env.subst("$PROJECT_DIR")),
        version=_backend_version(),
    )


if _PIO_ENV is not None:
    _PIO_ENV.AddPostAction(
        "$BUILD_DIR/${PROGNAME}.bin", _platformio_post_action
    )
