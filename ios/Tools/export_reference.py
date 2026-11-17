"""Refresh the iPhone identification index from the Android reference catalogs."""
import argparse
import filecmp
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
ANDROID = ROOT / "android/app/src/main"
SOURCE = ANDROID / "java/com/friendorfoe/presentation/util"
OUTPUT = ROOT / "ios/FriendOrFoe/More/ReferenceIndex.json"
PHOTO_OUTPUTS = ROOT / "ios/Tools/ReferencePhotoOutputs.xcfilelist"
RIGHTS = ROOT / "android/reference-rights/verified-photos.json"
CREDIT_FIELDS = ("title", "creator", "license", "licenseURL", "sourceURL", "changes")


def verified_photos(assets):
    manifest = json.loads(RIGHTS.read_text())
    if manifest["schemaVersion"] != 1:
        raise ValueError("Unsupported reference photo rights manifest")
    photos = {}
    for row in manifest["photos"]:
        name = row["path"]
        source = assets / name
        if (not re.fullmatch(r"(?:aircraft|drones)/[A-Za-z0-9_.-]+\.jpg", name)
                or name in photos or source.is_symlink() or not source.is_file()):
            raise ValueError(f"Invalid or missing verified photo: {name}")
        if hashlib.sha256(source.read_bytes()).hexdigest() != row["sha256"]:
            raise ValueError(f"Reference photo does not match the verified file: {name}")
        credit = {key: row[key] for key in CREDIT_FIELDS}
        if (not all(isinstance(value, str) for value in credit.values())
                or not all(credit[key].strip() for key in CREDIT_FIELDS if key != "changes")
                or not all(credit[key].startswith("https://") for key in ("sourceURL", "licenseURL"))):
            raise ValueError(f"Incomplete reference photo credit: {name}")
        photos[name] = credit
    return photos


def build_index():
    verified = verified_photos(ANDROID / "assets")
    policy = (SOURCE / "ReferencePhotoCatalog.kt").read_text()
    rejected = set(re.findall(r'"((?:aircraft|drones)/[^"]+)" to', policy))
    aircraft_exclusions = re.search(r'unavailableAircraftReferences = setOf\((.*?)\)', policy, re.S)
    hidden_ids = set(re.findall(r'"([^"]+)"', aircraft_exclusions[1]))
    entries = []
    for kind in ["Aircraft", "Drone"]:
        source = (SOURCE / f"{kind}Database.kt").read_text()
        groups = dict(re.findall(r'^\s+([A-Z0-9_]+)\("([^"]+)"\)', source, re.M))
        rows = re.split(r'^\s+' + kind + r'Reference\(\s*$', source, flags=re.M)[1:]
        for row in rows:
            def field(key):
                return json.loads(re.search(r'\b' + key + r'\s*=\s*("(?:\\.|[^"\\])*")', row, re.M)[1])

            identifier = field('id')
            category = re.search(r'category\s*=\s*\w+Category\.(\w+)', row)[1]
            data = dict(id=f"{kind}/{identifier}", kind=kind, name=field('name'),
                        manufacturer=field('manufacturer'), group=groups[category])
            if kind == 'Aircraft':
                codes = re.search(r'icaoTypeCodes\s*=\s*listOf\((.*?)\)', row, re.S)
                data['codes'] = re.findall(r'"([^"]+)"', codes[1]) if codes else []
            photo = field('photoAsset')
            allowed = photo not in rejected and (identifier not in hidden_ids if kind == "Aircraft" else identifier != "hoverair_x1_pro")
            if allowed and photo in verified:
                data['photo'] = photo
                data['credit'] = verified[photo]
            entries.append(data)
    if len({row['id'] for row in entries}) != len(entries):
        raise ValueError("Duplicate reference entry")
    return json.dumps(sorted(entries, key=lambda r: (r['kind'], r['name'].casefold())), ensure_ascii=False, indent=2) + '\n'


def bundle_photos(index_path, assets, bundle):
    """Synchronize the app's two photo directories with the approved index."""
    if bundle.suffix != ".app":
        raise ValueError("Expected an iPhone .app resource directory")
    verified = verified_photos(assets)
    names = {"aircraft": set(), "drones": set()}
    for item in json.loads(index_path.read_text()):
        if "photo" not in item:
            continue
        photo = Path(item["photo"])
        if len(photo.parts) != 2 or photo.parts[0] not in names or photo.name in {".", ".."}:
            raise ValueError(f"Invalid reference photo path: {photo}")
        if photo.as_posix() not in verified or item.get("credit") != verified[photo.as_posix()]:
            raise ValueError(f"Unverified or outdated credit for reference photo: {photo}")
        names[photo.parts[0]].add(photo.name)

    for group, approved in names.items():
        target = bundle / group
        if target.is_symlink():
            raise ValueError(f"Unexpected symbolic link in app resources: {target}")
        target.mkdir(parents=True, exist_ok=True)
        for entry in target.iterdir():
            if entry.name not in approved or entry.is_symlink() or entry.is_dir():
                if entry.is_dir() and not entry.is_symlink():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
        for name in sorted(approved):
            source = assets / group / name
            output = target / name
            if not output.exists() or not filecmp.cmp(source, output, shallow=False):
                shutil.copyfile(source, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--bundle-photos", metavar="APP", type=Path)
    args = parser.parse_args()
    if args.bundle_photos:
        bundle_photos(OUTPUT, ANDROID / "assets", args.bundle_photos)
    else:
        index = build_index()
        photos = sorted({row["photo"] for row in json.loads(index) if "photo" in row})
        outputs = "".join(f"$(TARGET_BUILD_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/{photo}\n" for photo in photos)
        if args.check:
            if OUTPUT.read_text() != index or PHOTO_OUTPUTS.read_text() != outputs:
                raise SystemExit("Run ios/Tools/export_reference.py to refresh the index and Xcode photo list")
        else:
            OUTPUT.write_text(index)
            PHOTO_OUTPUTS.write_text(outputs)
