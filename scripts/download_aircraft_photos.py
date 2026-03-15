#!/usr/bin/env python3
"""
Download aircraft photos from Wikimedia Commons for each ICAO type code.
Saves web-optimized JPEGs to images/aircraft/<TYPE_CODE>.jpg

Wikimedia requires a descriptive User-Agent with contact info.
See: https://meta.wikimedia.org/wiki/User-Agent_policy

Usage: python scripts/download_aircraft_photos.py
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

# Map ICAO type codes to search terms for Wikimedia Commons
TYPE_CODE_SEARCH = {
    # Narrowbody
    "B738": "Boeing 737-800",
    "B737": "Boeing 737-700",
    "B739": "Boeing 737-900",
    "B38M": "Boeing 737 MAX 8",
    "B39M": "Boeing 737 MAX 9",
    "A320": "Airbus A320",
    "A321": "Airbus A321",
    "A319": "Airbus A319",
    "A20N": "Airbus A320neo",
    "A21N": "Airbus A321neo",
    "B752": "Boeing 757-200",
    "B753": "Boeing 757-300",
    # Widebody
    "B77W": "Boeing 777-300ER",
    "B772": "Boeing 777-200",
    "B77L": "Boeing 777-200LR",
    "B788": "Boeing 787-8 Dreamliner",
    "B789": "Boeing 787-9 Dreamliner",
    "B78X": "Boeing 787-10 Dreamliner",
    "A332": "Airbus A330-200",
    "A333": "Airbus A330-300",
    "A339": "Airbus A330-900neo",
    "A359": "Airbus A350-900",
    "A35K": "Airbus A350-1000",
    "A388": "Airbus A380",
    "B744": "Boeing 747-400",
    "B748": "Boeing 747-8",
    "B763": "Boeing 767-300",
    "B764": "Boeing 767-400",
    # Regional
    "CRJ2": "Bombardier CRJ-200",
    "CRJ7": "Bombardier CRJ-700",
    "CRJ9": "Bombardier CRJ-900",
    "CRJX": "Bombardier CRJ-1000",
    "E170": "Embraer E170",
    "E75L": "Embraer E175",
    "E75S": "Embraer E175",
    "E190": "Embraer E190",
    "E195": "Embraer E195",
    "E290": "Embraer E190-E2",
    "E295": "Embraer E195-E2",
    # Turboprop
    "AT72": "ATR 72",
    "AT76": "ATR 72-600",
    "AT43": "ATR 42",
    "DH8A": "Dash 8-100",
    "DH8B": "Dash 8-200",
    "DH8C": "Dash 8-300",
    "DH8D": "Dash 8-400",
    "BE20": "Beechcraft King Air 200",
    "BE30": "Beechcraft King Air 300",
    "C208": "Cessna 208 Caravan",
    "PC12": "Pilatus PC-12",
    "SW4": "Fairchild Metroliner",
    # Bizjet
    "GLF4": "Gulfstream IV",
    "GLF5": "Gulfstream V",
    "GLF6": "Gulfstream G650",
    "GLEX": "Bombardier Global Express",
    "CL35": "Bombardier Challenger 350",
    "CL60": "Bombardier Challenger 600",
    "C56X": "Cessna Citation Excel",
    "C560": "Cessna Citation V",
    "C680": "Cessna Citation Sovereign",
    "C700": "Cessna Citation Longitude",
    "LJ35": "Learjet 35",
    "LJ45": "Learjet 45",
    "LJ60": "Learjet 60",
    "FA7X": "Dassault Falcon 7X",
    "FA8X": "Dassault Falcon 8X",
    "E55P": "Embraer Phenom 300",
    "HDJT": "Honda HA-420 HondaJet",
    "C510": "Cessna Citation Mustang",
    "GA5C": "Gulfstream G500",
    "GA6C": "Gulfstream G600",
    # Helicopter
    "R44": "Robinson R44",
    "R22": "Robinson R22",
    "EC35": "Eurocopter EC135",
    "EC45": "Eurocopter EC145",
    "EC30": "Eurocopter EC130",
    "AS50": "Eurocopter AS350",
    "A109": "AgustaWestland AW109",
    "A139": "AgustaWestland AW139",
    "B06": "Bell 206",
    "B407": "Bell 407",
    "B429": "Bell 429",
    "S76": "Sikorsky S-76",
    "S92": "Sikorsky S-92",
    "BK17": "Kawasaki BK117",
    # Fighter / military
    "F16": "General Dynamics F-16 Fighting Falcon",
    "F15": "McDonnell Douglas F-15 Eagle",
    "F18": "McDonnell Douglas F/A-18 Hornet",
    "FA18": "Boeing F/A-18E Super Hornet",
    "F22": "Lockheed Martin F-22 Raptor",
    "F35": "Lockheed Martin F-35 Lightning",
    "EUFI": "Eurofighter Typhoon",
    "RFAL": "Dassault Rafale",
    "B1": "Rockwell B-1 Lancer",
    "B2": "Northrop Grumman B-2 Spirit",
    "B52": "Boeing B-52 Stratofortress",
    "T6": "Beechcraft T-6 Texan II",
    "T38": "Northrop T-38 Talon",
    "T45": "Boeing T-45 Goshawk",
    "A10": "Fairchild Republic A-10 Thunderbolt",
    "C130H": "Lockheed C-130 Hercules",
    "F117": "Lockheed F-117 Nighthawk",
    "AV8B": "AV-8B Harrier II",
    "EA18": "Boeing EA-18G Growler",
    "E2C": "Grumman E-2 Hawkeye",
    "E3CF": "Boeing E-3 Sentry AWACS",
    "E6B": "Boeing E-6 Mercury",
    "KC10": "McDonnell Douglas KC-10 Extender",
    "KC46": "Boeing KC-46 Pegasus",
    "KC135": "Boeing KC-135 Stratotanker",
    # Cargo
    "C130": "Lockheed C-130 Hercules",
    "C17": "Boeing C-17 Globemaster III",
    "C5": "Lockheed C-5 Galaxy",
    "C5M": "Lockheed C-5M Super Galaxy",
    "A400": "Airbus A400M Atlas",
    "A400M": "Airbus A400M Atlas",
    "IL76": "Ilyushin Il-76",
    "AN124": "Antonov An-124 Ruslan",
    "C295": "Airbus C-295",
    # Lightplane
    "C172": "Cessna 172 Skyhawk",
    "C182": "Cessna 182 Skylane",
    "C152": "Cessna 152",
    "P28A": "Piper PA-28 Cherokee",
    "PA28": "Piper PA-28 Cherokee",
    "PA32": "Piper PA-32 Cherokee Six",
    "C210": "Cessna 210 Centurion",
    "C206": "Cessna 206 Stationair",
    "BE36": "Beechcraft Bonanza",
    "BE58": "Beechcraft Baron",
    "DA40": "Diamond DA40 Star",
    "DA42": "Diamond DA42 Twin Star",
    "SR20": "Cirrus SR20",
    "SR22": "Cirrus SR22",
    "M20P": "Mooney M20",
}

OUTPUT_DIR = Path(__file__).parent.parent / "images" / "aircraft"


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
            # Retry once
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
        "srsearch": f"{query} aircraft",
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
    total = len(TYPE_CODE_SEARCH)
    downloaded = 0
    skipped = 0
    failed = 0
    failed_codes = []

    for i, (code, search_term) in enumerate(TYPE_CODE_SEARCH.items(), 1):
        output_path = OUTPUT_DIR / f"{code}.jpg"
        if output_path.exists() and output_path.stat().st_size > 1000:
            print(f"[{i}/{total}] {code}: Already exists ({output_path.stat().st_size // 1024} KB), skipping")
            skipped += 1
            continue

        print(f"[{i}/{total}] {code}: Searching for '{search_term}'...", flush=True)

        # Step 1: Search
        file_title = search_commons(search_term)
        if not file_title:
            print(f"  No image found")
            failed += 1
            failed_codes.append(code)
            time.sleep(3)
            continue

        print(f"  Found: {file_title}", flush=True)
        time.sleep(2)  # pause between API calls

        # Step 2: Get URL
        image_url, credit = get_image_info(file_title)
        if not image_url:
            print(f"  Could not get URL")
            failed += 1
            failed_codes.append(code)
            time.sleep(3)
            continue

        time.sleep(2)  # pause between API calls

        # Step 3: Download
        if download_image(image_url, output_path):
            size_kb = output_path.stat().st_size / 1024
            print(f"  OK: {size_kb:.0f} KB", flush=True)
            credits[code] = {"search_term": search_term, "credit": credit}
            downloaded += 1
        else:
            failed += 1
            failed_codes.append(code)

        # Longer pause between full cycles
        time.sleep(5)

    # Also load credits for previously downloaded (skipped) files
    # by re-checking them
    print("\nUpdating credits for all files...", flush=True)
    all_jpgs = sorted(OUTPUT_DIR.glob("*.jpg"))
    for jpg in all_jpgs:
        code = jpg.stem
        if code not in credits and code in TYPE_CODE_SEARCH:
            credits[code] = {
                "search_term": TYPE_CODE_SEARCH[code],
                "credit": "Wikimedia Commons (see source file)"
            }

    # Write credits file
    credits_path = OUTPUT_DIR / "CREDITS.md"
    with open(credits_path, "w") as f:
        f.write("# Aircraft Photo Credits\n\n")
        f.write("All photos sourced from [Wikimedia Commons](https://commons.wikimedia.org/).\n")
        f.write("Licensed under Creative Commons or Public Domain.\n\n")
        f.write("| Type Code | Aircraft | Credit |\n")
        f.write("|-----------|----------|--------|\n")
        for code in sorted(credits.keys()):
            info = credits[code]
            credit = info["credit"].replace("|", "/")
            f.write(f"| {code} | {info['search_term']} | {credit} |\n")

    print(f"\nDone! Downloaded: {downloaded}, Skipped: {skipped}, Failed: {failed}")
    if failed_codes:
        print(f"Failed codes: {', '.join(failed_codes)}")
    print(f"Credits: {credits_path}")


if __name__ == "__main__":
    main()
