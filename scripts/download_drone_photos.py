#!/usr/bin/env python3
"""
Download drone photos from Wikimedia Commons for the drone reference database.
Saves web-optimized JPEGs to android/app/src/main/assets/drones/<key>.jpg

Wikimedia requires a descriptive User-Agent with contact info.
See: https://meta.wikimedia.org/wiki/User-Agent_policy

Usage: python scripts/download_drone_photos.py
"""

import json
import os
import re
import sys
import time
import urllib.request
import urllib.parse
from pathlib import Path

# Wikimedia requires contact info in User-Agent
USER_AGENT = "FriendOrFoeApp/1.0 (https://github.com/lnxgod/friendorfoe; lnxgod@gmail.com) Python/3"

# Map drone keys to search terms for Wikimedia Commons
DRONE_SEARCH = {
    # Consumer drones
    "dji_mavic3": "DJI Mavic 3 drone",
    "dji_mini4": "DJI Mini 4 Pro drone",
    "dji_phantom4": "DJI Phantom 4 drone",
    "dji_inspire3": "DJI Inspire 3 drone",
    "dji_fpv": "DJI FPV drone",
    "dji_matrice": "DJI Matrice 300 RTK drone",
    "dji_air": "DJI Air 3 drone",
    "skydio2": "Skydio 2 drone",
    "skydio_x10": "Skydio X10 drone",
    "parrot_anafi": "Parrot Anafi drone",
    "autel_evo2": "Autel EVO II drone",
    "autel_evo_nano": "Autel EVO Nano drone",
    "holy_stone": "Holy Stone drone quadcopter",
    "hubsan": "Hubsan Zino drone",
    "yuneec_typhoon": "Yuneec Typhoon H drone",
    "fimi_x8": "FIMI X8 drone Xiaomi",
    "hoverair_x1": "HOVERAir X1 selfie drone",
    "tello": "Ryze Tello DJI drone",
    # Military / threat drones
    "shahed_136": "Shahed 136 drone",
    "shahed_129": "Shahed 129 drone Iran",
    "mohajer_6": "Mohajer-6 drone Iran",
    "bayraktar_tb2": "Bayraktar TB2 drone",
    "bayraktar_akinci": "Bayraktar Akinci drone",
    "orion_uav": "Orion drone Russia UAV",
    "lancet": "ZALA Lancet drone",
    "wing_loong": "Wing Loong II drone China",
    "mq9_reaper": "MQ-9 Reaper drone",
    "mq1_predator": "MQ-1 Predator drone",
    "rq4_global_hawk": "RQ-4 Global Hawk drone",
    "switchblade": "AeroVironment Switchblade drone",
    "heron": "IAI Heron drone Israel",
}

OUTPUT_DIR = Path(__file__).parent.parent / "android" / "app" / "src" / "main" / "assets" / "drones"


def api_request(url: str) -> dict | None:
    """Make a Wikimedia API request with proper User-Agent."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        if e.code == 429:
            retry_after = int(e.headers.get("Retry-After", 60))
            print(f"  Rate limited. Waiting {retry_after}s...")
            time.sleep(retry_after)
            try:
                req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=30) as resp:
                    return json.loads(resp.read())
            except Exception:
                return None
        print(f"  HTTP Error {e.code}")
        return None
    except Exception as e:
        print(f"  Request error: {e}")
        return None


def search_commons(query: str) -> str | None:
    """Search Wikimedia Commons for an image and return the file title."""
    params = urllib.parse.urlencode({
        "action": "query",
        "list": "search",
        "srsearch": query,
        "srnamespace": "6",
        "srlimit": "5",
        "format": "json",
    })
    data = api_request(f"https://commons.wikimedia.org/w/api.php?{params}")
    if not data:
        return None
    results = data.get("query", {}).get("search", [])
    for result in results:
        title = result.get("title", "")
        lower = title.lower()
        if any(lower.endswith(ext) for ext in [".jpg", ".jpeg", ".png"]):
            return title
    return None


def get_image_info(file_title: str) -> tuple[str | None, str | None]:
    """Get the actual image URL and credit from a Wikimedia Commons file title."""
    params = urllib.parse.urlencode({
        "action": "query",
        "titles": file_title,
        "prop": "imageinfo",
        "iiprop": "url|extmetadata",
        "iiurlwidth": "800",
        "format": "json",
    })
    data = api_request(f"https://commons.wikimedia.org/w/api.php?{params}")
    if not data:
        return None, None
    pages = data.get("query", {}).get("pages", {})
    for page in pages.values():
        imageinfo = page.get("imageinfo", [{}])[0]
        thumb_url = imageinfo.get("thumburl", imageinfo.get("url"))
        metadata = imageinfo.get("extmetadata", {})
        artist = metadata.get("Artist", {}).get("value", "Unknown")
        license_name = metadata.get("LicenseShortName", {}).get("value", "Unknown")
        artist = re.sub(r"<[^>]+>", "", artist).strip()
        commons_url = imageinfo.get("descriptionurl", "")
        return thumb_url, f"{artist} | {license_name} | {commons_url}"
    return None, None


def download_image(url: str, output_path: Path) -> bool:
    """Download an image to the specified path."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=30) as resp:
            with open(output_path, "wb") as f:
                f.write(resp.read())
        return True
    except Exception as e:
        print(f"  Download error: {e}")
        return False


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    credits = {}
    total = len(DRONE_SEARCH)
    downloaded = 0
    skipped = 0
    failed = 0
    failed_keys = []

    for i, (key, search_term) in enumerate(DRONE_SEARCH.items(), 1):
        output_path = OUTPUT_DIR / f"{key}.jpg"
        if output_path.exists() and output_path.stat().st_size > 1000:
            print(f"[{i}/{total}] {key}: Already exists ({output_path.stat().st_size // 1024} KB), skipping")
            skipped += 1
            continue

        print(f"[{i}/{total}] {key}: Searching for '{search_term}'...", flush=True)

        # Step 1: Search
        file_title = search_commons(search_term)
        if not file_title:
            print(f"  No image found")
            failed += 1
            failed_keys.append(key)
            time.sleep(3)
            continue

        print(f"  Found: {file_title}", flush=True)
        time.sleep(2)

        # Step 2: Get URL
        image_url, credit = get_image_info(file_title)
        if not image_url:
            print(f"  Could not get URL")
            failed += 1
            failed_keys.append(key)
            time.sleep(3)
            continue

        time.sleep(2)

        # Step 3: Download
        if download_image(image_url, output_path):
            size_kb = output_path.stat().st_size / 1024
            print(f"  OK: {size_kb:.0f} KB", flush=True)
            credits[key] = {"search_term": search_term, "credit": credit}
            downloaded += 1
        else:
            failed += 1
            failed_keys.append(key)

        time.sleep(5)

    # Load credits for skipped files
    print("\nUpdating credits for all files...", flush=True)
    all_jpgs = sorted(OUTPUT_DIR.glob("*.jpg"))
    for jpg in all_jpgs:
        key = jpg.stem
        if key not in credits and key in DRONE_SEARCH:
            credits[key] = {
                "search_term": DRONE_SEARCH[key],
                "credit": "Wikimedia Commons (see source file)"
            }

    # Write credits file
    credits_path = OUTPUT_DIR / "CREDITS.md"
    with open(credits_path, "w") as f:
        f.write("# Drone Photo Credits\n\n")
        f.write("All photos sourced from [Wikimedia Commons](https://commons.wikimedia.org/).\n")
        f.write("Licensed under Creative Commons or Public Domain.\n\n")
        f.write("| Key | Drone | Credit |\n")
        f.write("|-----|-------|--------|\n")
        for key in sorted(credits.keys()):
            info = credits[key]
            credit = info["credit"].replace("|", "/")
            f.write(f"| {key} | {info['search_term']} | {credit} |\n")

    print(f"\nDone! Downloaded: {downloaded}, Skipped: {skipped}, Failed: {failed}")
    if failed_keys:
        print(f"Failed keys: {', '.join(failed_keys)}")
    print(f"Credits: {credits_path}")


if __name__ == "__main__":
    main()
