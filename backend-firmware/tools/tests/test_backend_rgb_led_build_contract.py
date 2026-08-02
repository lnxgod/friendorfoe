"""Focused build-artifact contracts for profile-specific status LEDs."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN_LITE_ARTIFACT_MARKERS = (
    "backend_fullsize_rgb_led",
    "led_strip",
    "fullsize-components/backend_fullsize_led",
)


@pytest.mark.parametrize(
    ("project", "project_name", "profile_name", "expected"),
    (
        ("scanner", "fof_backend_scanner_fullsize", "badge_lite", "mismatch"),
        ("uplink", "fof_backend_uplink", "s3_fullsize", "mismatch"),
        ("scanner", "not_a_backend_project", "unknown", "Unknown backend scanner profile"),
        ("uplink", "not_a_backend_project", "unknown", "Unknown backend uplink profile"),
    ),
)
def test_backend_roots_reject_unknown_or_mismatched_profile_projects(
    tmp_path: Path,
    project: str,
    project_name: str,
    profile_name: str,
    expected: str,
) -> None:
    """A wrong selector must never choose an LED adapter accidentally."""
    result = subprocess.run(
        [
            "cmake",
            "-S",
            str(ROOT / project),
            "-B",
            str(tmp_path / project),
            f"-DFOF_BACKEND_PROJECT_NAME={project_name}",
            f"-DFOF_BACKEND_PROFILE_NAME={profile_name}",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0
    assert expected in result.stdout + result.stderr


@pytest.mark.parametrize(
    ("project", "environment", "project_name"),
    (
        ("scanner", "scanner-s3-combo-backend", "fof_backend_scanner"),
        ("uplink", "uplink-s3-backend", "fof_backend_uplink"),
    ),
)
def test_lite_build_artifacts_exclude_the_fullsize_rgb_component(
    project: str, environment: str, project_name: str
) -> None:
    """Lite must not compile, link, or embed the Fullsize LED dependency tree."""
    build = ROOT / project / ".pio/build" / environment
    compilation_database = build / "compile_commands.json"
    link_map = build / f"{project_name}.map"
    app = build / "firmware.elf"
    assert compilation_database.is_file(), f"missing build artifact: {compilation_database}"
    assert link_map.is_file(), f"missing build artifact: {link_map}"
    assert app.is_file(), f"missing build artifact: {app}"

    compilation_commands = json.loads(compilation_database.read_text(encoding="utf-8"))
    compilation_text = "\n".join(
        " ".join(str(entry.get(key, "")) for key in ("directory", "command", "file"))
        for entry in compilation_commands
    )
    link_text = link_map.read_text(encoding="utf-8", errors="replace")
    app_text = app.read_bytes().decode("latin-1")
    for marker in FORBIDDEN_LITE_ARTIFACT_MARKERS:
        assert marker not in compilation_text
        assert marker not in link_text
        assert marker not in app_text
