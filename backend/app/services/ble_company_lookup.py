"""BLE Company ID → manufacturer + category lookup.

Uses the Nordic Semiconductor bluetooth-numbers-database (4000+ entries).
Falls back to a built-in subset if the JSON file isn't available.
"""

import json
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

# Device category keywords mapped from company names
_CATEGORY_KEYWORDS = {
    "Drone": ["dji", "parrot", "skydio", "autel", "yuneec", "flyability", "wingcopter", "freefly"],
    "Vehicle": ["tesla", "ford motor", "toyota", "bmw", "mercedes", "rivian", "volkswagen",
                "general motors", "hyundai", "honda motor", "nissan", "subaru", "kia"],
    "Smart Home": ["ring ", "arlo", "wyze", "nest ", "ecobee", "signify", "hue", "ikea",
                   "tuya", "govee", "meross", "shelly", "chamberlain", "august", "yale",
                   "lutron", "honeywell", "lifx", "rachio", "irobot", "neato"],
    "Wearable": ["fitbit", "garmin", "polar electro", "suunto", "whoop", "oura", "amazfit",
                 "huami", "peloton", "coros"],
    "Audio": ["bose", "harman", "jbl", "sennheiser", "jabra", "beats electronics",
              "sonos", "bang & olufsen", "skullcandy", "anker innovations", "soundcore",
              "sony ", "marshall"],
    "Gaming": ["nintendo", "playstation", "xbox", "valve corp", "razer", "oculus"],
    "Medical": ["omron", "medtronic", "dexcom", "abbott", "roche", "withings",
                "tandem diabetes", "insulet", "resmed"],
    "Tracker": ["tile,", "tile inc", "chipolo", "pebblebee", "tracmo"],
    "Phone": ["samsung electronics", "xiaomi", "huawei", "oppo", "oneplus", "motorola",
              "lg electronics", "zte", "vivo mobile", "realme"],
}

# Loaded company ID database
_company_db: dict[int, dict] = {}  # code → {name, category}


def _categorize(name: str) -> str:
    """Determine device category from company name."""
    name_lower = name.lower()
    for category, keywords in _CATEGORY_KEYWORDS.items():
        if any(kw in name_lower for kw in keywords):
            return category
    return ""


def load_company_ids():
    """Load the Nordic BLE company ID database."""
    global _company_db
    json_path = Path(__file__).parent / "ble_company_ids.json"

    try:
        with open(json_path) as f:
            data = json.load(f)
        for entry in data:
            code = entry.get("code", 0)
            name = entry.get("name", "Unknown")
            _company_db[code] = {
                "name": name,
                "category": _categorize(name),
            }
        logger.info("Loaded %d BLE company IDs (%d categorized)",
                    len(_company_db),
                    sum(1 for v in _company_db.values() if v["category"]))
    except Exception as e:
        logger.warning("Failed to load BLE company IDs: %s", e)
        # Minimal fallback
        _company_db = {
            0x004C: {"name": "Apple", "category": "Phone"},
            0x0075: {"name": "Samsung", "category": "Phone"},
            0x00E0: {"name": "Google", "category": "Phone"},
            0x009E: {"name": "Bose", "category": "Audio"},
            0x0087: {"name": "Garmin", "category": "Wearable"},
            0x08AA: {"name": "DJI", "category": "Drone"},
            0x02E5: {"name": "Espressif", "category": ""},
            0x0059: {"name": "Nordic Semi", "category": ""},
        }


def lookup_company(company_id: int) -> tuple[str, str]:
    """Look up company name and category from BLE company ID.

    Returns (company_name, category) or ("Unknown", "").
    """
    if not _company_db:
        load_company_ids()
    entry = _company_db.get(company_id)
    if entry:
        return entry["name"], entry["category"]
    return "Unknown", ""


def get_company_count() -> int:
    """Return number of loaded company IDs."""
    if not _company_db:
        load_company_ids()
    return len(_company_db)


# Auto-load on import
load_company_ids()
