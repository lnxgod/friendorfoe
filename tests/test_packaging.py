import pathlib
import tomllib
import unittest


class PackagingTest(unittest.TestCase):
    def test_pyserial_is_the_only_runtime_dependency(self) -> None:
        root = pathlib.Path(__file__).parents[1]
        project = tomllib.loads((root / "pyproject.toml").read_text())
        self.assertEqual(project["project"]["requires-python"], ">=3.11")
        self.assertEqual(project["project"]["dependencies"], ["pyserial>=3.5,<4"])
