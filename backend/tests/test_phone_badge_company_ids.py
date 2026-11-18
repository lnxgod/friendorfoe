"""Check brand attribution for literal Bluetooth company-ID rules against the offline SIG registry."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_company_id_classifiers_name_the_registered_vendor():
    registry = json.loads((ROOT / "backend/app/data/rf_reference.json").read_text())["ble_company_ids"]
    detector = (ROOT / "android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt").read_text()
    firmware = (ROOT / "esp32/scanner/main/detection/glasses_detector.c").read_text()
    table = firmware.split("s_mfr_cid_db[] = {", 1)[1].split("};", 1)[0]
    android_ids = re.findall(r'MfrEntry\(0x([a-fA-F0-9]+),\s*"([^"]+)"', detector)
    badge_ids = re.findall(r'\{\s*0x([a-fA-F0-9]+),\s*"([^"]+)"', table)
    assert android_ids and badge_ids
    # Legacy legal names and documented manufacturing partners, scoped to the exact assignment.
    aliases = {0x034D: "taser", 0x0D53: "luxottica"}
    for raw, brand in android_ids + badge_ids:
        code = int(raw, 16)
        registrant = registry[str(code)]["name"].casefold()
        assert aliases.get(code, brand.casefold()) in registrant, (raw, brand, registrant)
