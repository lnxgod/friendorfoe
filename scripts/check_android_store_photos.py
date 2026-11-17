"""Compare the actual distribution package with the shared, reviewed photo manifest."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
from zipfile import ZipFile

MANIFEST = Path(__file__).resolve().parents[1] / "android/reference-rights/verified-photos.json"


def verify(package):
    approved = MANIFEST.read_bytes()
    records = json.loads(approved)["photos"]
    with ZipFile(package) as archive:
        prefix = "base/assets/" if package.suffix == ".aab" else "assets/"
        if archive.read(prefix + "verified-photos.json") != approved:
            raise ValueError("The package does not carry the reviewed credits.")
        actual = {item[len(prefix):] for item in archive.namelist()
                  if item.startswith((prefix + "aircraft/", prefix + "drones/")) and not item.endswith("/")}
        expected = {record["path"] for record in records}
        if actual != expected or not expected or len(records) != len(expected):
            raise ValueError(f"Unexpected or missing reference assets: {sorted(actual ^ expected)}")
        for record in records:
            if sha256(archive.read(prefix + record["path"])).hexdigest() != record["sha256"]:
                raise ValueError(f"Changed photo: {record['path']}")
    print(f"Verified {len(records)} packaged reference photos and their credits.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    verify(parser.parse_args().package)
