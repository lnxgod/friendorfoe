import pathlib
import stat
import subprocess
import tomllib
import unittest


class PackagingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = pathlib.Path(__file__).parents[1]

    def test_pyserial_is_the_only_runtime_dependency(self) -> None:
        project = tomllib.loads((self.root / "pyproject.toml").read_text())
        self.assertEqual(project["project"]["requires-python"], ">=3.11")
        self.assertEqual(project["project"]["dependencies"], ["pyserial>=3.5,<4"])

    def test_source_bootstrap_is_executable_and_stays_within_new_dash(self) -> None:
        script_path = self.root / "run.sh"

        self.assertTrue(script_path.is_file())
        self.assertTrue(script_path.stat().st_mode & stat.S_IXUSR)
        source = script_path.read_text(encoding="utf-8")
        self.assertIn("set -eu", source)
        self.assertIn("NEW_DASH_DIR", source)
        self.assertIn('dirname "$0"', source)
        self.assertIn('"$NEW_DASH_DIR/.venv"', source)
        self.assertIn('-m pip install -e "$NEW_DASH_DIR"', source)
        self.assertIn('-m new_dash "$@"', source)
        self.assertNotIn("source ", source)
        self.assertNotIn(".env", source)
        self.assertNotIn("backend", source)
        syntax = subprocess.run(
            ["sh", "-n", str(script_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)

    def test_package_data_patterns_cover_every_dashboard_asset(self) -> None:
        project = tomllib.loads((self.root / "pyproject.toml").read_text())
        patterns = project["tool"]["setuptools"]["package-data"]["new_dash"]
        source_assets = {
            path.relative_to(self.root / "src" / "new_dash").as_posix()
            for path in (self.root / "src" / "new_dash" / "static").rglob("*")
            if path.is_file()
        }
        uncovered = {
            asset
            for asset in source_assets
            if not any(pathlib.PurePosixPath(asset).match(pattern) for pattern in patterns)
        }

        self.assertEqual(uncovered, set())
