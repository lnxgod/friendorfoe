"""Curated drone Wi-Fi/BLE signature tables for Friend or Foe.

This module is designed as a paste-ready reference layer for detector code.
It mirrors the existing ESP32/backend tables where possible and expands them
into Python data structures with manufacturer/model metadata.

Primary source roots used when curating this file:
- Local repo tables:
  - esp32/scanner/main/detection/wifi_ssid_patterns.c
  - esp32/scanner/main/detection/wifi_oui_database.c
  - esp32/scanner/main/detection/dji_drone_id_parser.c
  - backend/app/services/enrichment_ble.py
  - backend/app/services/ble_company_ids.json
- Bluetooth SIG Assigned Numbers (company identifiers)
- Official DJI product/support/spec pages
- Official CaddxFPV / Walksnail pages
- Official HDZero docs/shop pages

Important caveats:
- "All known" is not realistically provable; this is a best-effort curated set
  aligned to the current codebase as of 2026-03-29.
- Some prefixes identify controllers, goggles, payloads, or telemetry radios,
  not only aircraft. The ``role`` field makes that explicit.
- Some OUIs are 24-bit MA-L assignments, while some vendors also hold MA-M or
  MA-S allocations. Because your scanner keys off the first 3 bytes, this file
  keeps 24-bit prefixes only and flags ambiguous chip-vendor prefixes.
- DJI has an official Bluetooth SIG company ID (0x08AA) but the current
  ``ble_fingerprint.c`` also treats 0x2CA5 as an observed DJI manufacturer-data
  discriminator. Both are preserved below.
"""

from __future__ import annotations

from typing import Any


_RAW_WIFI_SSID_PREFIX_TO_MANUFACTURER: dict[str, str] = {
    "DJI-": "DJI",
    "TELLO-": "Ryze/DJI",
    "MAVIC-": "DJI",
    "PHANTOM-": "DJI",
    "INSPIRE-": "DJI",
    "MINI SE-": "DJI",
    "MINI2-": "DJI",
    "MINI3-": "DJI",
    "MINI4-": "DJI",
    "SPARK-": "DJI",
    "FPV-": "DJI",
    "AVATA-": "DJI",
    "AGRAS-": "DJI",
    "MATRICE-": "DJI",
    "AIR 2S-": "DJI",
    "AIR2-": "DJI",
    "FLIP-": "DJI",
    "DJI NEO-": "DJI",
    "SKYDIO-": "Skydio",
    "PARROT-": "Parrot",
    "ANAFI-": "Parrot",
    "BEBOP-": "Parrot",
    "DISCO-": "Parrot",
    "ARDRONE-": "Parrot",
    "AUTEL-": "Autel",
    "EVO-": "Autel",
    "HOVERAIR": "HOVERAir",
    "HOVER AIR": "HOVERAir",
    "HOVER_AIR": "HOVERAir",
    "HOVER-AIR": "HOVERAir",
    "HOVERAir": "HOVERAir",
    "HOVER X1": "HOVERAir",
    "HOVER-X1": "HOVERAir",
    "HOVER_X1": "HOVERAir",
    "X1PRO": "HOVERAir",
    "X1-PRO": "HOVERAir",
    "X1 PRO": "HOVERAir",
    "HOLY": "Holy Stone",
    "HS-": "Holy Stone",
    "SIMREX-": "SIMREX",
    "NEHEME-": "Neheme",
    "AOVO-": "AOVO",
    "TENSSENX-": "TENSSENX",
    "SNAPTAIN-": "Snaptain",
    "POTENSIC-": "Potensic",
    "RUKO-": "Ruko",
    "SYMA-": "Syma",
    "HUBSAN-": "Hubsan",
    "EACHINE-": "Eachine",
    "FIMI-": "Fimi",
    "XIAOMI-": "Xiaomi",
    "YUNEEC-": "Yuneec",
    "TYPHOON-": "Yuneec",
    "MANTIS-": "Yuneec",
    "WINGSLAND-": "Wingsland",
    "BETAFPV-": "BetaFPV",
    "GEPRC-": "GEPRC",
    "EMAX-": "EMAX",
    "POWEREGG-": "PowerVision",
    "DOBBY-": "ZEROTECH",
    "SPLASHDRONE-": "Swellpro",
    "CONTIXO-": "Contixo",
    "SKYVIPER-": "Sky Viper",
    "DROCON-": "Drocon",
    "FREEFLY-": "Freefly",
    "SENSEFLY-": "senseFly",
    "WINGCOPTER-": "Wingcopter",
    "FLYABILITY-": "Flyability",
    "IFLIGHT-": "iFlight",
    "FLYWOO-": "Flywoo",
    "WALKERA-": "Walkera",
    "BLADE-": "Blade",
    "CADDX-": "Caddx",
    "WALKSNAIL-": "Walksnail",
    "AVATAR-": "Walksnail",
    "RUNCAM-": "RunCam",
    "WIFI-UAV": "Generic",
    "WIFI_UAV": "Generic",
    "WIFIUAV": "Generic",
    "WiFi-720P": "Generic",
    "WiFi-1080P": "Generic",
    "WiFi-4K": "Generic",
    "WIFI_CAMERA": "Generic",
    "WiFi_FPV": "Generic",
    "WiFi-FPV": "Generic",
    "RCDrone": "Generic",
    "RC-DRONE": "Generic",
    "RCTOY": "Generic",
    "UFO-": "Generic",
    "JJRC-": "JJRC",
    "MJX-": "MJX",
    "VISUO-": "Visuo",
    "SJRC-": "SJRC",
    "4DRC-": "4DRC",
    "FLYHAL-": "Flyhal",
    "LYZRC-": "LYZRC",
    "XINLIN-": "Xinlin",
    "E58-": "Eachine",
    "E88-": "Eachine",
    "E99-": "Eachine",
    "V2PRO": "Generic",
    "WLTOYS-": "WLtoys",
    "ATTOP-": "Attop",
    "BUGS-": "MJX",
    "EHANG-": "EHang",
    "DRONE-": "Unknown",
    "UAV-": "Unknown",
    "QUADCOPTER-": "Unknown",
    "FPV_WIFI": "Generic",
    "FPV-WIFI": "Generic",
    "WIFI FPV": "Generic",
    "DJI-Mini4Pro-": "DJI",
    "DJI-Air3-": "DJI",
    "DJI-Mavic3Classic-": "DJI",
    "DJI-Avata2-": "DJI",
    "DJI-Neo-": "DJI",
    "DJI_FPV_": "DJI",
    "DJI_Goggles_": "DJI",
    "DJI-Goggles3-": "DJI",
    "RID-": "DJI",
    "avatarx_": "Walksnail",
    "avatar_rx_": "Walksnail",
    "hd0": "HDZero",
    "HDZero": "HDZero",
    "WiFiUFO-": "Generic",
    "Wi-Fi UFO-": "Generic",
    "WIFI UFO-": "Generic",
    "GM-WiFiUFO": "Generic",
    "Wifi_Drone_": "Generic",
    "DEERC-": "DEERC",
    "DeercFPV-": "DEERC",
    "4DRC": "4DRC",
    "Bwine-F7-": "Ruko/Bwine",
    "LW FPV-": "Eachine",
    "SJ-GPS": "SJRC",
    "SJF Pro_": "SJRC",
    "SG906": "ZLRC",
    "Beast-": "ZLRC",
    "CSJ-GPS-": "CSJ",
    "HolyStoneEIS-": "Holy Stone",
    "Potensic D_": "Potensic",
    "RUKO-F11-": "Ruko",
    "RUKO-PRO-": "Ruko",
    "Controller-": "Generic",
    "rededge": "MicaSense",
    "Sequoia_": "Parrot",
    "SKYVIPERGPS_": "Sky Viper",
    "SKYVIPER17_": "Sky Viper",
    "SKY VIPER_": "Sky Viper",
    "Force1_": "Force1",
    "RMTT-": "Ryze/DJI",
    "iFly-": "Generic",
    "FH8610UFO-": "Generic",
    "FH8610-": "Generic",
    "ht-ufo_": "Generic",
    "HolyStoneFPV_": "Holy Stone",
    "Potensic_": "Potensic",
    "Eachine_": "Eachine",
    "EggX_": "PowerVision",
    "Solo_": "3DR",
    "sololink_": "3DR",
    "Silvus-": "Silvus Technologies",
    "Silvus_": "Silvus Technologies",
    "MPU5-": "Persistent Systems",
    "HoverCamera_": "Zero Zero Robotics",
    "PowerUp-": "PowerUp Toys",
    "PRA_Station_": "PowerVision",
    "PSE_": "PowerVision",
    "Gladius_5G_": "CHASING",
    "Gladius_2.4G_": "CHASING",
    "Chasing_": "CHASING",
    "M2_": "CHASING",
    "FIFISHRC_": "QYSEA",
    "FIFISH RC_": "QYSEA",
    "Open.HD": "OpenHD",
    "ExpressLRS TX": "ELRS",
    "ExpressLRS RX": "ELRS",
    "TBS_XF_AP_": "TBS",
    "TBS_TR_AP_": "TBS",
    "TBS_Fusion_AP_": "TBS",
    "RunCam_": "RunCam",
    "RunCam2_": "RunCam",
    "Herelink": "CubePilot",
    "PixRacer": "ArduPilot",
    "DJI_Smart_Controller_": "DJI",
    "DJI-RC-": "DJI",
    "Skydio2-": "Skydio",
    "Skydio2+-": "Skydio",
    "SkydioX10-": "Skydio",
    "DT_": "Dedrone",
    "ACS2": "XAG",
}


_EXACT_SSID_MODEL_OVERRIDES: dict[str, str] = {
    "DJI-": "Generic DJI Wi-Fi / QuickTransfer / pairing SSID",
    "TELLO-": "Ryze Tello",
    "RMTT-": "RoboMaster TT / Tello EDU family",
    "MAVIC-": "DJI Mavic family",
    "PHANTOM-": "DJI Phantom family",
    "INSPIRE-": "DJI Inspire family",
    "MINI SE-": "DJI Mini SE",
    "MINI2-": "DJI Mini 2 / Mini 2 SE",
    "MINI3-": "DJI Mini 3 / Mini 3 Pro",
    "MINI4-": "DJI Mini 4 family",
    "SPARK-": "DJI Spark",
    "FPV-": "DJI FPV / digital FPV family",
    "AVATA-": "DJI Avata family",
    "AGRAS-": "DJI Agras family",
    "MATRICE-": "DJI Matrice family",
    "AIR 2S-": "DJI Air 2S",
    "AIR2-": "DJI Air 2 / Mavic Air 2 family",
    "FLIP-": "DJI Flip",
    "DJI NEO-": "DJI Neo",
    "DJI-Mini4Pro-": "DJI Mini 4 Pro QuickTransfer",
    "DJI-Air3-": "DJI Air 3 QuickTransfer",
    "DJI-Mavic3Classic-": "DJI Mavic 3 Classic QuickTransfer",
    "DJI-Avata2-": "DJI Avata 2 QuickTransfer",
    "DJI-Neo-": "DJI Neo QuickTransfer",
    "DJI_FPV_": "DJI FPV system component",
    "DJI_Goggles_": "DJI Goggles Wi-Fi / share mode",
    "DJI-Goggles3-": "DJI Goggles 3 Wi-Fi / share mode",
    "RID-": "DJI Remote ID / telemetry side-channel",
    "SKYDIO-": "Skydio generic aircraft",
    "Skydio2-": "Skydio 2",
    "Skydio2+-": "Skydio 2+",
    "SkydioX10-": "Skydio X10 / X10D family",
    "PARROT-": "Parrot generic aircraft/controller",
    "ANAFI-": "Parrot Anafi family",
    "BEBOP-": "Parrot Bebop family",
    "DISCO-": "Parrot Disco",
    "ARDRONE-": "Parrot AR.Drone family",
    "Sequoia_": "Parrot Sequoia multispectral payload",
    "AUTEL-": "Autel generic aircraft",
    "EVO-": "Autel EVO family",
    "HOVERAIR": "HOVERAir generic aircraft",
    "HOVER AIR": "HOVERAir generic aircraft",
    "HOVER_AIR": "HOVERAir generic aircraft",
    "HOVER-AIR": "HOVERAir generic aircraft",
    "HOVERAir": "HOVERAir generic aircraft",
    "HOVER X1": "HOVERAir X1",
    "HOVER-X1": "HOVERAir X1",
    "HOVER_X1": "HOVERAir X1",
    "X1PRO": "HOVERAir X1 Pro",
    "X1-PRO": "HOVERAir X1 Pro",
    "X1 PRO": "HOVERAir X1 Pro",
    "POWEREGG-": "PowerVision PowerEgg",
    "DOBBY-": "ZEROTECH Dobby",
    "SPLASHDRONE-": "SwellPro SplashDrone",
    "SKYVIPERGPS_": "Sky Viper GPS series",
    "SKYVIPER17_": "Sky Viper 17-series",
    "SKY VIPER_": "Sky Viper generic aircraft",
    "Force1_": "Force1 toy-drone family",
    "Herelink": "CubePilot Herelink air/ground unit",
    "PixRacer": "PixRacer / ArduPilot telemetry node",
    "rededge": "MicaSense RedEdge payload",
    "Gladius_5G_": "CHASING Gladius 5G ROV",
    "Gladius_2.4G_": "CHASING Gladius 2.4G ROV",
    "Chasing_": "CHASING underwater drone / ROV",
    "M2_": "CHASING M2 ROV",
    "FIFISHRC_": "QYSEA FIFISH ROV",
    "FIFISH RC_": "QYSEA FIFISH ROV",
    "Open.HD": "OpenHD digital FPV link",
    "ExpressLRS TX": "ExpressLRS transmitter / backpack AP",
    "ExpressLRS RX": "ExpressLRS receiver / backpack AP",
    "TBS_XF_AP_": "TBS Crossfire AP / backpack",
    "TBS_TR_AP_": "TBS Tracer AP / backpack",
    "TBS_Fusion_AP_": "TBS Fusion AP / backpack",
    "RunCam_": "RunCam Wi-Fi camera",
    "RunCam2_": "RunCam 2 Wi-Fi camera",
    "DJI_Smart_Controller_": "DJI Smart Controller",
    "DJI-RC-": "DJI RC / RC Pro controller",
    "DT_": "Dedrone counter-UAS sensor",
    "ACS2": "XAG agricultural control station",
    "avatarx_": "Walksnail Avatar goggles / share AP",
    "avatar_rx_": "Walksnail Avatar VRX / receiver AP",
    "HDZero": "HDZero goggles / expansion Wi-Fi",
    "hd0": "HDZero goggles / expansion Wi-Fi",
    "Bwine-F7-": "Bwine F7",
    "LW FPV-": "Eachine LW FPV series",
    "SJ-GPS": "SJRC GPS family",
    "SJF Pro_": "SJRC SJF Pro family",
    "SG906": "ZLRC SG906 Beast family",
    "Beast-": "ZLRC Beast family",
    "CSJ-GPS-": "CSJ GPS family",
    "HolyStoneEIS-": "Holy Stone EIS family",
    "Potensic D_": "Potensic Dreamer family",
    "RUKO-F11-": "Ruko F11 family",
    "RUKO-PRO-": "Ruko Pro family",
    "HoverCamera_": "Hover Camera / Zero Zero Robotics family",
    "PRA_Station_": "PowerVision base station",
    "PSE_": "PowerVision aircraft / ROV support node",
    "PowerUp-": "PowerUp smart paper airplane platform",
    "EggX_": "PowerVision Egg X",
}


_BUDGET_OR_TOY_BRANDS = {
    "Holy Stone",
    "SIMREX",
    "Neheme",
    "AOVO",
    "TENSSENX",
    "Snaptain",
    "Potensic",
    "Ruko",
    "Syma",
    "Hubsan",
    "Eachine",
    "Wingsland",
    "JJRC",
    "MJX",
    "Visuo",
    "SJRC",
    "4DRC",
    "Flyhal",
    "LYZRC",
    "Xinlin",
    "WLtoys",
    "Attop",
    "DEERC",
    "Ruko/Bwine",
    "ZLRC",
    "CSJ",
    "Force1",
    "Contixo",
    "Drocon",
}

_FPV_BRANDS = {
    "BetaFPV",
    "GEPRC",
    "EMAX",
    "iFlight",
    "Flywoo",
    "Walkera",
    "Blade",
    "Caddx",
    "Walksnail",
    "RunCam",
    "HDZero",
    "OpenHD",
    "ELRS",
    "TBS",
}

_ENTERPRISE_BRANDS = {
    "Freefly",
    "senseFly",
    "Wingcopter",
    "Flyability",
    "MicaSense",
    "XAG",
    "Dedrone",
}


def _infer_ssid_metadata(prefix: str, manufacturer: str) -> dict[str, Any]:
    p = prefix.lower()
    role = "aircraft"
    segment = "consumer"
    confidence = 0.85

    if manufacturer in _BUDGET_OR_TOY_BRANDS:
        segment = "budget"
    elif manufacturer in _FPV_BRANDS:
        segment = "fpv"
    elif manufacturer in _ENTERPRISE_BRANDS:
        segment = "enterprise"
    elif manufacturer in {"CHASING", "QYSEA"}:
        segment = "underwater"
    elif manufacturer in {"Silvus Technologies", "Persistent Systems"}:
        segment = "datalink"

    if manufacturer in {"Generic", "Unknown"}:
        segment = "budget"
        role = "aircraft_or_camera"
        confidence = 0.3 if manufacturer == "Generic" else 0.2
    elif "controller" in p or p.startswith("dji-rc-") or "smart_controller" in p:
        role = "controller"
    elif "goggles" in p or p.startswith("avatarx_") or p.startswith("avatar_rx_") or prefix in {"HDZero", "hd0"}:
        role = "goggles_or_vrx"
    elif manufacturer == "MicaSense":
        role = "camera_payload"
    elif manufacturer in {"Silvus Technologies", "Persistent Systems"}:
        role = "datalink"
    elif manufacturer == "Dedrone":
        role = "counter_uas"
        confidence = 0.7

    if prefix in _EXACT_SSID_MODEL_OVERRIDES:
        model = _EXACT_SSID_MODEL_OVERRIDES[prefix]
    elif manufacturer in _BUDGET_OR_TOY_BRANDS:
        model = f"{manufacturer} consumer / toy drone family"
    elif manufacturer in _FPV_BRANDS:
        model = f"{manufacturer} FPV aircraft / camera / receiver family"
    elif manufacturer in {"Freefly", "senseFly", "Wingcopter", "Flyability"}:
        model = f"{manufacturer} enterprise aircraft family"
    elif manufacturer == "PowerVision":
        model = "PowerVision aircraft / ROV family"
    elif manufacturer == "Ryze/DJI":
        model = "Ryze / DJI educational or entry platform"
    elif manufacturer == "DJI":
        model = "DJI aircraft / accessory family"
    elif manufacturer == "Skydio":
        model = "Skydio aircraft family"
    elif manufacturer == "Parrot":
        model = "Parrot aircraft family"
    elif manufacturer == "Autel":
        model = "Autel aircraft family"
    elif manufacturer == "HOVERAir":
        model = "HOVERAir aircraft family"
    elif manufacturer == "Yuneec":
        model = "Yuneec aircraft family"
    elif manufacturer == "EHang":
        model = "EHang aircraft / AAV family"
    elif manufacturer == "3DR":
        model = "3DR Solo / SoloLink family"
    else:
        model = f"{manufacturer} aircraft / accessory family"

    generic_wifi_prefixes = (
        "wifi-uav",
        "wifi_uav",
        "wifiuav",
        "wifi-720p",
        "wifi-1080p",
        "wifi-4k",
        "wifi_camera",
        "wifi_fpv",
        "wifi-fpv",
        "rcdrone",
        "rc-drone",
        "rctoy",
        "ufo-",
        "fpv_wifi",
        "fpv-wifi",
        "wifi fpv",
        "wifiufo-",
        "wi-fi ufo-",
        "wifi ufo-",
        "gm-wifiufo",
        "wifi_drone_",
        "ifly-",
        "fh8610ufo-",
        "fh8610-",
        "ht-ufo_",
    )
    if p.startswith(generic_wifi_prefixes) or prefix in {"V2PRO", "Controller-"}:
        model = "Generic Wi-Fi FPV / toy drone module"
        segment = "budget"
        role = "aircraft_or_camera"
        confidence = 0.3

    return {
        "manufacturer": manufacturer,
        "model": model,
        "segment": segment,
        "role": role,
        "confidence": confidence,
    }


DRONE_WIFI_SSID_PREFIXES: dict[str, dict[str, Any]] = {
    prefix: _infer_ssid_metadata(prefix, manufacturer)
    for prefix, manufacturer in _RAW_WIFI_SSID_PREFIX_TO_MANUFACTURER.items()
}


SOFT_GENERIC_WIFI_SSID_RULES: dict[str, Any] = {
    "prefix_alnum_tail_patterns": {
        "WIFI_": {"tail_min": 1, "tail_max": 8},
        "FPV_": {"tail_min": 1, "tail_max": 8},
        "CAMERA_": {"tail_min": 1, "tail_max": 8},
    },
    "exact_prefixes": ("4K_CAM", "4KCAM", "RCFPV"),
    "max_ssid_length": 16,
    "confidence": 0.15,
}


_DRONE_RELATED_MANUFACTURERS = (
    "DJI",
    "Ryze",
    "Skydio",
    "Parrot",
    "Autel",
    "HOVERAir",
    "Holy Stone",
    "SIMREX",
    "Neheme",
    "AOVO",
    "TENSSENX",
    "Snaptain",
    "Potensic",
    "Ruko",
    "Syma",
    "Hubsan",
    "Eachine",
    "Fimi",
    "Xiaomi",
    "Yuneec",
    "Wingsland",
    "BetaFPV",
    "GEPRC",
    "EMAX",
    "PowerVision",
    "ZEROTECH",
    "Swellpro",
    "Contixo",
    "Sky Viper",
    "Drocon",
    "Freefly",
    "senseFly",
    "Wingcopter",
    "Flyability",
    "iFlight",
    "Flywoo",
    "Walkera",
    "Blade",
    "Caddx",
    "Walksnail",
    "RunCam",
    "JJRC",
    "MJX",
    "Visuo",
    "SJRC",
    "4DRC",
    "Flyhal",
    "LYZRC",
    "Xinlin",
    "WLtoys",
    "Attop",
    "EHang",
    "HDZero",
    "DEERC",
    "Ruko/Bwine",
    "ZLRC",
    "CSJ",
    "MicaSense",
    "Force1",
    "3DR",
    "Silvus Technologies",
    "Persistent Systems",
    "Zero Zero Robotics",
    "PowerUp Toys",
    "CHASING",
    "QYSEA",
    "OpenHD",
    "ELRS",
    "TBS",
    "CubePilot",
    "ArduPilot",
    "Dedrone",
    "XAG",
)


DRONE_BLE_COMPANY_IDS: dict[str, dict[str, Any]] = {
    manufacturer: {
        "company_id": None,
        "official_name": None,
        "assigned_by_bluetooth_sig": False,
        "observed_company_ids": [],
        "notes": "No explicit Bluetooth SIG company-identifier match found in current Assigned Numbers snapshot.",
    }
    for manufacturer in _DRONE_RELATED_MANUFACTURERS
}

DRONE_BLE_COMPANY_IDS.update(
    {
        "DJI": {
            "company_id": 0x08AA,
            "official_name": "SZ DJI TECHNOLOGY CO.,LTD",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [0x2CA5],
            "notes": "0x08AA is the official Bluetooth SIG company identifier; 0x2CA5 is preserved because current repo captures/fingerprint code treat it as a DJI manufacturer-data discriminator.",
        },
        "Parrot": {
            "company_id": 0x0043,
            "official_name": "PARROT AUTOMOTIVE SAS",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [],
            "notes": "Assigned Numbers uses a Parrot affiliate/legal-entity name rather than 'Parrot SA'.",
        },
        "Xiaomi": {
            "company_id": 0x038F,
            "official_name": "Xiaomi Inc.",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [],
            "notes": "Useful for Xiaomi/FIMI ecosystem devices.",
        },
        "Fimi": {
            "company_id": 0x038F,
            "official_name": "Xiaomi Inc.",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [],
            "notes": "No separate FIMI company identifier found; Xiaomi corporate ID is the closest official match.",
        },
        "Zero Zero Robotics": {
            "company_id": 0x09F3,
            "official_name": "Beijing Zero Zero Infinity Technology Co.,Ltd.",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [],
            "notes": "Official company identifier for the Zero Zero / HOVERAir corporate entity.",
        },
        "HOVERAir": {
            "company_id": 0x09F3,
            "official_name": "Beijing Zero Zero Infinity Technology Co.,Ltd.",
            "assigned_by_bluetooth_sig": True,
            "observed_company_ids": [],
            "notes": "Brand alias for Zero Zero Robotics.",
        },
    }
)


DRONE_WIFI_OUIS: dict[str, dict[str, Any]] = {
    "DJI": {
        "official_24bit_prefixes": [
            "60:60:1F",
            "34:D2:62",
            "48:1C:B9",
            "08:D4:6A",
            "D0:32:9A",
            "C4:2F:90",
        ],
        "observed_additional_prefixes": [
            "8C:58:23",
            "0C:9A:E6",
            "E4:7A:2C",
            "88:29:85",
            "58:B8:58",
            "04:A8:5A",
            "4C:43:F6",
            "9C:5A:8A",
            "EC:72:F7",
        ],
        "high_false_positive": False,
    },
    "Parrot": {
        "official_24bit_prefixes": ["A0:14:3D", "90:03:B7", "00:12:1C", "00:26:7E"],
        "observed_additional_prefixes": ["90:3A:E6"],
        "high_false_positive": False,
    },
    "Autel": {
        "official_24bit_prefixes": ["2C:DC:AD", "78:8C:B5"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Skydio": {
        "official_24bit_prefixes": ["58:D5:6E"],
        "observed_additional_prefixes": ["38:1D:14"],
        "high_false_positive": False,
    },
    "Yuneec": {
        "official_24bit_prefixes": ["EC:D0:9F", "64:D4:DA"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "HOVERAir": {
        "official_24bit_prefixes": ["10:D0:7A"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Xiaomi": {
        "official_24bit_prefixes": ["28:6C:07", "64:CE:01"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "FIMI": {
        "official_24bit_prefixes": ["9C:99:A0"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Hubsan": {
        "official_24bit_prefixes": ["D8:96:E0"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Holy Stone": {
        "official_24bit_prefixes": ["CC:DB:A7"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Potensic": {
        "official_24bit_prefixes": ["B0:A7:32"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Walkera": {
        "official_24bit_prefixes": ["C8:14:51"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Syma": {
        "official_24bit_prefixes": ["E8:AB:FA"],
        "observed_additional_prefixes": [],
        "high_false_positive": False,
    },
    "Generic/ESP": {
        "official_24bit_prefixes": ["24:0A:C4", "30:AE:A4", "A4:CF:12", "AC:67:B2"],
        "observed_additional_prefixes": [],
        "high_false_positive": True,
    },
    "Generic/Realtek": {
        "official_24bit_prefixes": ["00:E0:4C"],
        "observed_additional_prefixes": [],
        "high_false_positive": True,
    },
}


DJI_PROTOCOL_SIGNATURES: dict[str, dict[str, Any]] = {
    "wifi_droneid_vendor_ie": {
        "description": "DJI DroneID / Enhanced Wi-Fi vendor-specific IE carried inside 802.11 beacon frames.",
        "ie_tag": 221,
        "oui": "26:37:12",
        "endianness": "little-endian fields after the OUI",
        "min_payload_bytes_after_oui": 29,
        "observed_transmit_interval_ms": 200,
        "message_types": {
            0x10: "Telemetry / aircraft + home-point location packet",
            0x11: "User-entered / flight-metadata packet",
        },
        "repo_parser_layout": {
            "byte_0": "version_or_type",
            "bytes_1_4": "serial_prefix_ascii",
            "bytes_5_8": "drone_longitude_deg_x1e7_int32",
            "bytes_9_12": "drone_latitude_deg_x1e7_int32",
            "bytes_13_14": "altitude_m_int16",
            "bytes_15_16": "height_above_ground_dm_int16",
            "bytes_17_18": "speed_cmps_uint16",
            "bytes_19_20": "heading_deg_x100_int16",
            "bytes_21_24": "home_longitude_deg_x1e7_int32",
            "bytes_25_28": "home_latitude_deg_x1e7_int32",
        },
        "detection_for_esp32": {
            "works_in_promiscuous_beacon_capture": True,
            "requires_standard_80211_beacons": True,
            "best_signal": "Vendor IE with OUI 26:37:12 in beacon frame body",
        },
    },
    "ocusync_o3_o3_plus": {
        "description": "DJI proprietary digital video/control link family used by FPV and newer consumer systems. Not standard Wi-Fi.",
        "bands_mhz": [(2400.0, 2483.5), (5725.0, 5850.0)],
        "communication_bandwidth_mhz": [20, 40],
        "known_product_examples": [
            "DJI O3 Air Unit",
            "DJI FPV Goggles V2",
            "DJI FPV Remote Controller 2",
        ],
        "detectable_as_80211_by_esp32": False,
        "best_esp32_side_channels": [
            "DJI Wi-Fi SSIDs for QuickTransfer/share mode",
            "DJI BLE names such as DJI-RC-*",
            "DJI BLE company IDs 0x08AA / observed 0x2CA5",
            "DJI OUIs",
        ],
    },
    "o4_air_unit_family": {
        "description": "DJI O4 digital FPV link family. Also not standard Wi-Fi.",
        "bands_mhz": [(2400.0, 2483.5), (5150.0, 5250.0), (5725.0, 5850.0)],
        "communication_bandwidth_mhz": [10, 20, 40, 60],
        "documented_5_8ghz_channel_centers_mhz": [5768.5, 5789.5, 5794.5, 5814.5],
        "known_product_examples": [
            "DJI O4 Air Unit",
            "DJI O4 Air Unit Pro",
            "DJI Goggles 3",
            "DJI Goggles N3",
            "DJI FPV Remote Controller 3",
        ],
        "detectable_as_80211_by_esp32": False,
        "best_esp32_side_channels": [
            "DJI_Goggles_* and DJI-Goggles3-* Wi-Fi share-mode SSIDs",
            "DJI-RC-* BLE advertisements",
            "DJI QuickTransfer SSIDs from associated aircraft",
            "DJI OUIs",
        ],
    },
    "ble_side_channels": {
        "official_company_id": 0x08AA,
        "observed_company_ids": [0x2CA5],
        "observed_name_prefixes": [
            "DJI-RC-",
            "DJI RC",
            "DJI_Goggles_",
            "DJI-Goggles3-",
        ],
        "note": "In practice, controllers and goggles are more commonly BLE-visible than the air unit itself.",
    },
}


DIGITAL_FPV_SIGNATURES: dict[str, dict[str, Any]] = {
    "dji_digital_fpv": {
        "manufacturer": "DJI",
        "air_link_type": "OcuSync / O3 / O4 proprietary digital link",
        "standard_80211_beacons_expected": False,
        "esp32_direct_decode_possible": False,
        "ssid_side_channels": [
            "DJI_FPV_",
            "DJI_Goggles_",
            "DJI-Goggles3-",
            "DJI-Mini4Pro-",
            "DJI-Air3-",
            "DJI-Mavic3Classic-",
            "DJI-Avata2-",
            "DJI-Neo-",
        ],
        "ble_side_channels": {
            "company_ids": [0x08AA, 0x2CA5],
            "name_prefixes": ["DJI-RC-"],
        },
        "rf_energy_heuristic": {
            "look_for_energy_only_without_beacons": True,
            "bands_mhz": [(2400.0, 2483.5), (5150.0, 5250.0), (5725.0, 5850.0)],
            "comment": "For true OcuSync/O4 air-link detection you need SDR/spectrum capability; ESP32 alone mostly sees side-channels.",
        },
    },
    "walksnail_avatar": {
        "manufacturer": "Walksnail / Caddx",
        "air_link_type": "Proprietary digital FPV link",
        "standard_80211_beacons_expected": False,
        "esp32_direct_decode_possible": False,
        "documented_band_mhz": [(5725.0, 5850.0)],
        "documented_channels": 8,
        "ssid_side_channels": ["WALKSNAIL-", "AVATAR-", "avatarx_", "avatar_rx_"],
        "ble_side_channels": {
            "company_ids": [],
            "name_prefixes": [],
        },
        "rf_energy_heuristic": {
            "look_for_energy_only_without_beacons": True,
            "comment": "Walksnail air units do not present as normal Wi-Fi APs; detect via side-channel SSIDs or external RF energy sensing.",
        },
    },
    "hdzero": {
        "manufacturer": "HDZero",
        "air_link_type": "Digital FPV broadcast link",
        "standard_80211_beacons_expected": False,
        "esp32_direct_decode_possible": False,
        "documented_band_mhz": [(5650.0, 5925.0)],
        "ssid_side_channels": ["HDZero", "hd0"],
        "ble_side_channels": {
            "company_ids": [],
            "name_prefixes": [],
        },
        "rf_energy_heuristic": {
            "look_for_energy_only_without_beacons": True,
            "comment": "HDZero air link is not standard Wi-Fi; ESP32 usually only detects Wi-Fi created by goggles/modules/share features, not the VTX itself.",
        },
    },
}


def match_drone_wifi_ssid(ssid: str | None) -> dict[str, Any] | None:
    """Case-insensitive prefix match against the curated SSID table."""
    if not ssid:
        return None
    ssid_lower = ssid.lower()
    for prefix, metadata in sorted(
        DRONE_WIFI_SSID_PREFIXES.items(),
        key=lambda item: len(item[0]),
        reverse=True,
    ):
        if ssid_lower.startswith(prefix.lower()):
            return {"matched_prefix": prefix, **metadata}
    return None


def match_soft_generic_drone_ssid(ssid: str | None) -> dict[str, Any] | None:
    """Soft-match cheap/generic drone hotspots.

    Mirrors the logic in ``wifi_ssid_match_soft()`` from the ESP32 code:
    - reject SSIDs longer than 16 characters
    - ``WIFI_``, ``FPV_``, ``CAMERA_`` followed by 1-8 alphanumeric chars
    - exact-prefix matches for ``4K_CAM``, ``4KCAM``, ``RCFPV``
    """
    if not ssid or len(ssid) > SOFT_GENERIC_WIFI_SSID_RULES["max_ssid_length"]:
        return None

    for prefix, rule in SOFT_GENERIC_WIFI_SSID_RULES["prefix_alnum_tail_patterns"].items():
        if ssid.lower().startswith(prefix.lower()):
            tail = ssid[len(prefix):]
            if rule["tail_min"] <= len(tail) <= rule["tail_max"] and tail.isalnum():
                return {
                    "matched_prefix": prefix,
                    "manufacturer": "Generic",
                    "model": "Generic Wi-Fi FPV / toy drone module",
                    "segment": "budget",
                    "role": "aircraft_or_camera",
                    "confidence": SOFT_GENERIC_WIFI_SSID_RULES["confidence"],
                }

    for prefix in SOFT_GENERIC_WIFI_SSID_RULES["exact_prefixes"]:
        if ssid.lower().startswith(prefix.lower()):
            return {
                "matched_prefix": prefix,
                "manufacturer": "Generic",
                "model": "Generic Wi-Fi FPV / toy drone module",
                "segment": "budget",
                "role": "aircraft_or_camera",
                "confidence": SOFT_GENERIC_WIFI_SSID_RULES["confidence"],
            }
    return None


def lookup_drone_wifi_oui(prefix3: str | None) -> tuple[str, dict[str, Any]] | None:
    """Look up a manufacturer from the first 3 MAC bytes (XX:XX:XX)."""
    if not prefix3:
        return None
    key = prefix3.upper()
    for manufacturer, data in DRONE_WIFI_OUIS.items():
        if key in data["official_24bit_prefixes"] or key in data["observed_additional_prefixes"]:
            return manufacturer, data
    return None
