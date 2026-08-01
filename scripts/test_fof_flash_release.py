import importlib.util
from pathlib import Path

import pytest


MODULE_PATH = Path(__file__).with_name("fof_flash.py")


def _load_module():
    spec = importlib.util.spec_from_file_location("fof_flash_release_test", MODULE_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _write_version_header(repo_root: Path, body: str) -> None:
    header = repo_root / "esp32" / "shared" / "version.h"
    header.parent.mkdir(parents=True)
    header.write_text(body)


def test_current_repo_version_selects_literal_production_track(tmp_path):
    module = _load_module()
    _write_version_header(
        tmp_path,
        '\n'.join(
            (
                '#define FOF_VERSION_PROD  "0.64.68-live-follow"',
                '#define FOF_VERSION_BADGE "0.64.76-badge-defcon34"',
                '#define FOF_VERSION FOF_VERSION_BADGE',
            )
        ),
    )
    module.REPO_ROOT = tmp_path

    assert module.current_repo_firmware_version() == "0.64.68-live-follow"


@pytest.mark.parametrize(
    "header",
    (
        '#define FOF_VERSION FOF_VERSION_PROD',
        '#define FOF_VERSION_PROD FOF_VERSION_BADGE',
        '#define FOF_VERSION_PROD ""',
        '#define FOF_VERSION_PROD "unknown"',
    ),
)
def test_current_repo_version_fails_closed_without_literal_release_version(
    tmp_path,
    header,
):
    module = _load_module()
    _write_version_header(tmp_path, header)
    module.REPO_ROOT = tmp_path

    with pytest.raises(RuntimeError, match="FOF_VERSION_PROD"):
        module.current_repo_firmware_version()
