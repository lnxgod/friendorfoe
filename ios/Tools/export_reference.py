"""Refresh the iPhone identification index from the Android reference catalogs."""
import argparse
import filecmp
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
ANDROID = ROOT / "android/app/src/main"
SOURCE = ANDROID / "java/com/friendorfoe/presentation/util"
OUTPUT = ROOT / "ios/FriendOrFoe/More/ReferenceIndex.json"


def build_index():
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
            if allowed and (ANDROID / "assets" / photo).is_file():
                data['photo'] = photo
            entries.append(data)
    if len({row['id'] for row in entries}) != len(entries):
        raise ValueError("Duplicate reference entry")
    return json.dumps(sorted(entries, key=lambda r: (r['kind'], r['name'].casefold())), ensure_ascii=False, indent=2) + '\n'


def bundle_photos(index_path, assets, bundle):
    """Synchronize the app's two photo directories with the approved index."""
    if bundle.suffix != ".app":
        raise ValueError("Expected an iPhone .app resource directory")
    names = {"aircraft": set(), "drones": set()}
    for item in json.loads(index_path.read_text()):
        if "photo" not in item:
            continue
        photo = Path(item["photo"])
        if len(photo.parts) != 2 or photo.parts[0] not in names or photo.name in {".", ".."}:
            raise ValueError(f"Invalid reference photo path: {photo}")
        source = assets / photo
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"Missing reference photo: {photo}")
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
    elif args.check:
        if OUTPUT.read_text() != build_index():
            raise SystemExit("Run ios/Tools/export_reference.py to refresh the index")
    else:
        OUTPUT.write_text(build_index())
