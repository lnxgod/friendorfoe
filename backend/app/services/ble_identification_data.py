"""BLE identification data for device fingerprinting.

Provides:
  - SERVICE_UUIDS: 16-bit Service UUID → description (GATT + SDO + Member services)
  - COMPANY_IDS: 16-bit Company Identifier → company name (curated major brands)
  - FAST_PAIR_MODELS: 24-bit Fast Pair Model ID → device name (curated popular devices)

Notes:
  - The Bluetooth SIG "Member Services" table lists the requesting company, not the service name.
    For those UUIDs we expose the company name and add a few well-known overrides (Fast Pair, etc).
"""

from __future__ import annotations

import re
from typing import Final, Literal

__all__ = [
    "SERVICE_UUIDS",
    "COMPANY_IDS",
    "FAST_PAIR_MODELS",
    "ble_random_address_subtype_bits",
    "ble_random_address_subtype",
    "looks_like_random_ble_address",
]


def _parse_uuid_name_table(text: str) -> dict[int, str]:
    """Parse lines like: `0xFE2C Google LLC`.

    Also supports wrapped/continuation lines (lines without a UUID) by appending them
    to the previous entry.
    """
    out: dict[int, str] = {}
    last_uuid: int | None = None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            continue

        m = re.match(r"^0x([0-9A-Fa-f]{4})\s+(.+)$", line)
        if m:
            last_uuid = int(m.group(1), 16)
            out[last_uuid] = m.group(2).strip()
            continue

        # Continuation (e.g., PDF extracted multi-line company names)
        if last_uuid is not None:
            out[last_uuid] = (out[last_uuid] + " " + line).strip()

    return out


_GATT_SERVICE_UUIDS_BY_UUID: Final[dict[int, str]] = {
    0x1800: "GAP Service",
    0x1801: "GATT Service",
    0x1802: "Immediate Alert Service",
    0x1803: "Link Loss Service",
    0x1804: "Tx Power Service",
    0x1805: "Current Time Service",
    0x1806: "Reference Time Update Service",
    0x1807: "Next DST Change Service",
    0x1808: "Glucose Service",
    0x1809: "Health Thermometer Service",
    0x180A: "Device Information Service",
    0x180D: "Heart Rate Service",
    0x180E: "Phone Alert Status Service",
    0x180F: "Battery Service",
    0x1810: "Blood Pressure Service",
    0x1811: "Alert Notification Service",
    0x1812: "Human Interface Device Service",
    0x1813: "Scan Parameters Service",
    0x1814: "Running Speed and Cadence Service",
    0x1815: "Automation IO Service",
    0x1816: "Cycling Speed and Cadence Service",
    0x1818: "Cycling Power Service",
    0x1819: "Location and Navigation Service",
    0x181A: "Environmental Sensing Service",
    0x181B: "Body Composition Service",
    0x181C: "User Data Service",
    0x181D: "Weight Scale Service",
    0x181E: "Bond Management Service",
    0x181F: "Continuous Glucose Monitoring Service",
    0x1820: "Internet Protocol Support Service",
    0x1821: "Indoor Positioning Service",
    0x1822: "Pulse Oximeter Service",
    0x1823: "HTTP Proxy Service",
    0x1824: "Transport Discovery Service",
    0x1825: "Object Transfer Service",
    0x1826: "Fitness Machine Service",
    0x1827: "Mesh Provisioning Service",
    0x1828: "Mesh Proxy Service",
    0x1829: "Reconnection Configuration Service",
    0x183A: "Insulin Delivery Service",
    0x183B: "Binary Sensor Service",
    0x183C: "Emergency Configuration Service",
    0x183D: "Authorization Control Service",
    0x183E: "Physical Activity Monitor Service",
    0x183F: "Elapsed Time Service",
    0x1840: "Generic Health Sensor Service",
    0x1843: "Audio Input Control Service",
    0x1844: "Volume Control Service",
    0x1845: "Volume Offset Control Service",
    0x1846: "Coordinated Set Identification Service",
    0x1847: "Device Time Service",
    0x1848: "Media Control Service",
    0x1849: "Generic Media Control Service",
    0x184A: "Constant Tone Extension Service",
    0x184B: "Telephone Bearer Service",
    0x184C: "Generic Telephone Bearer Service",
    0x184D: "Microphone Control Service",
    0x184E: "Audio Stream Control Service",
    0x184F: "Broadcast Audio Scan Service",
    0x1850: "Published Audio Capabilities Service",
    0x1851: "Basic Audio Announcement Service",
    0x1852: "Broadcast Audio Announcement Service",
    0x1853: "Common Audio Service",
    0x1854: "Hearing Access Service",
    0x1855: "Telephony and Media Audio Service",
    0x1856: "Public Broadcast Announcement Service",
    0x1857: "Electronic Shelf Label Service",
    0x1858: "Gaming Audio Service",
    0x1859: "Mesh Proxy Solicitation Service",
    0x185A: "Industrial Measurement Device Service",
    0x185B: "Ranging Service",
    0x185C: "HID ISO Service",
    0x185D: "Cookware Service",
    0x185E: "Voice Assistant Service",
    0x185F: "Generic Voice Assistant Service",
}


_SDO_SERVICE_UUIDS: Final[dict[int, str]] = {
    0xFCCC: "Wi‑Fi Easy Connect Specification Service (Wi‑Fi Alliance)",
    0xFFEF: "Wi‑Fi Direct Specification Service (Wi‑Fi Alliance)",
    0xFFF0: "Public Key Open Credential (PKOC) Service (Physical Security Interoperability Alliance)",
    0xFFF1: "ICCE Digital Key Service (China Association of Automobile Manufactures)",
    0xFFF2: "Aliro Service (Connectivity Standards Alliance)",
    0xFFF3: "FiRa Consortium Service (FiRa Consortium)",
    0xFFF4: "FiRa Consortium Service (FiRa Consortium)",
    0xFFF5: "Car Connectivity Consortium, LLC Service (Car Connectivity Consortium, LLC)",
    0xFFF6: "Matter Profile ID Service (Connectivity Standards Alliance)",
    0xFFF7: "Zigbee Direct Service (Connectivity Standards Alliance)",
    0xFFF8: "Mopria Alliance BLE Service (Mopria Alliance)",
    0xFFF9: "FIDO2 secure client‑to‑authenticator transport Service (FIDO)",
    0xFFFA: "ASTM Remote ID Service (ASTM International)",
    0xFFFB: "Direct Thread Commissioning Service (Thread Group, Inc.)",
    0xFFFC: "Wireless Power Transfer (WPT) Service (AirFuel Alliance)",
    0xFFFD: "Universal Second Factor Authenticator Service (FIDO)",
    0xFFFE: "Wireless Power Transfer Service (AirFuel Alliance)",
}


_MEMBER_SERVICE_UUIDS_TABLE: Final[str] = """
0xFC39 Harman International
0xFC3A C.O.B.O. SpA
0xFC3B BONX INC.
0xFC3C Eforthink Technology Co., Ltd.
0xFC3D Reelables, Inc.
0xFC3E Google LLC
0xFC3F Milwaukee Electric Tools
0xFC41 Yoto Limited
0xFC43 Atmosic Technologies, Inc.
0xFC44 Block, Inc.
0xFC45 Mitsubishi Electric Corporation
0xFC46 Xiaomi
0xFC47 Shanghai Ingeek Technology Co., Ltd.
0xFC48 Michelin
0xFC49 Golioth, Inc.
0xFC4A Shenzhen Shokz Co.,Ltd.
0xFC4B WinMagic Inc.
0xFC4C HP Inc.
0xFC4D Lodestar Technology Inc.
0xFC4E Lodestar Technology Inc.
0xFC4F WaveRF, Corp.
0xFC50 Ant Group Co., Ltd.
0xFC51 Ant Group Co., Ltd.
0xFC52 LG Electronics Inc.
0xFC53 LEGIC Identsystems AG
0xFC54 Shenzhen Yinwang Intelligent Technologies Co., Ltd.
0xFC55 BYD Company Limited
0xFC56 Google LLC
0xFC57 Ambient Life Inc.
0xFC58 Shenzhen Minew Technologies Co., Ltd.
0xFC59 Ant Group Co., Ltd.
0xFC5A LAST LOCK INC.
0xFC5B Time Location Systems AS
0xFC5C PLASTIC RESEARCH AND DEVELOPMENT CORPORATION
0xFC5D GP Acoustics International Limited
0xFC5E KUBU SMART LIMITED
0xFC5F PI-CRYSTAL INC.
0xFC60 Ohme Operations UK Limited
0xFC61 QIKCONNEX LLC
0xFC62 SPRiNTUS GmbH
0xFC63 Volvo Technology AB
0xFC64 Volvo Technology AB
0xFC65 Robor Electronics B.V.
0xFC66 Xiaomi Inc.
0xFC67 Guangdong Hengqin Xingtong Technology Co.,ltd.
0xFC68 RIGH, INC.
0xFC69 Harman International
0xFC6A Sonos Inc
0xFC6B Sonos Inc
0xFC6C Powerstick.com
0xFC6D MOTIVE TECHNOLOGIES, INC.
0xFC6E stryker
0xFC6F NextSense, Inc.
0xFC70 MOTIVE TECHNOLOGIES, INC.
0xFC71 Hive-Zox International SA
0xFC72 iodyne, LLC
0xFC73 Google LLC
0xFC74 EMBEINT INC
0xFC75 Xiaomi Inc.
0xFC76 Weber-Stephen Products LLC
0xFC77 SING SUN TECHNOLOGY (INTERNATIONAL) LIMITED
0xFC78 DHL
0xFC79 LG Electronics Inc.
0xFC7A Outshiny India Private Limited
0xFC7B Testo SE & Co. KGaA
0xFC7C Motorola Mobility, LLC
0xFC7D MML US, Inc
0xFC7E Harman International
0xFC7F Southco
0xFC80 TELE System Communications Pte. Ltd.
0xFC81 Axon Enterprise, Inc.
0xFC82 Zwift, Inc.
0xFC83 iHealth Labs, Inc.
0xFC84 NINGBO FOTILE KITCHENWARE CO., LTD.
0xFC85 Zhejiang Huanfu Technology Co., LTD
0xFC86 Samsara Networks, Inc
0xFC87 Samsara Networks, Inc
0xFC88 CCC del Uruguay
0xFC89 Intel Corporation
0xFC8A Intel Corporation
0xFC8B Kaspersky Lab Middle East FZ-LLC
0xFC8C VusionGroup
0xFC8D Caire Inc.
0xFC8E Blue Iris Labs, Inc.
0xFC8F Bose Corporation
0xFC90 Wiliot LTD.
0xFC91 Samsung Electronics Co., Ltd.
0xFC92 Furuno Electric Co., Ltd.
0xFC93 Komatsu Ltd.
0xFC94 Apple Inc.
0xFC95 Hippo Camp Software Ltd.
0xFC96 LEGO System A/S
0xFC97 Japan Display Inc.
0xFC98 Ruuvi Innovations Ltd.
0xFC99 Badger Meter
0xFC9A Koppli AB
0xFC9B Merry Electronics (S) Pte Ltd
0xFC9D Lenovo (Singapore) Pte Ltd.
0xFC9E Dell Computer Corporation
0xFC9F Delta Development Team, Inc
0xFCA0 Apple Inc.
0xFCA1 PF SCHWEISSTECHNOLOGIE GMBH
0xFCA2 Meizu Technology Co., Ltd.
0xFCA3 Gunnebo Aktiebolag
0xFCA4 HP Inc.
0xFCA5 HAYWARD INDUSTRIES, INC.
0xFCA6 Hubble Network Inc.
0xFCA7 Hubble Network Inc.
0xFCA8 Medtronic Inc.
0xFCA9 Medtronic Inc.
0xFCAA Spintly, Inc.
0xFCAB IRISS INC.
0xFCAC IRISS INC.
0xFCAD Beijing 99help Safety Technology Co., Ltd
0xFCAE Imagine Marketing Limited
0xFCAF AltoBeam Inc.
0xFCB0 Ford Motor Company
0xFCB1 Google LLC
0xFCB2 Apple Inc.
0xFCB3 SWEEN
0xFCB4 OMRON HEALTHCARE Co., Ltd.
0xFCB5 OMRON HEALTHCARE Co., Ltd.
0xFCB6 OMRON HEALTHCARE Co., Ltd.
0xFCB7 T-Mobile USA
0xFCB8 Ribbiot, INC.
0xFCB9 Lumi United Technology Co., Ltd
0xFCBA BlueID GmbH
0xFCBB SharkNinja Operating LLC
0xFCBC Drowsy Digital, Inc.
0xFCBD Toshiba Corporation
0xFCBE Musen Connect, Inc.
0xFCBF ASSA ABLOY Opening Solutions Sweden AB
0xFCC0 Xiaomi Inc.
0xFCC1 TIMECODE SYSTEMS LIMITED
0xFCC2 Qualcomm Technologies, Inc.
0xFCC3 HP Inc.
0xFCC4 OMRON(DALIAN) CO,.LTD.
0xFCC5 OMRON(DALIAN) CO,.LTD.
0xFCC6 Wiliot LTD.
0xFCC7 PB INC.
0xFCC8 Allthenticate, Inc.
0xFCC9 SkyHawke Technologies
0xFCCA Cosmed s.r.l.
0xFCCB TOTO LTD.
0xFCCD Marshall Group AB
0xFCCE Luna Health, Inc.
0xFCCF Google LLC
0xFCD0 Laerdal Medical AS
0xFCD1 Shenzhen Benwei Media Co.,Ltd.
0xFCD2 Allterco Robotics ltd
0xFCD3 Fisher & Paykel Healthcare
0xFCD4 OMRON HEALTHCARE
0xFCD5 Nortek Security & Control
0xFCD6 SWISSINNO SOLUTIONS AG
0xFCD7 PowerPal Pty Ltd
0xFCD8 Appex Factory S.L.
0xFCD9 Huso, INC
0xFCDA Draeger
0xFCDB aconno GmbH
0xFCDC Amazon.com Services, LLC
0xFCDD Mobilaris AB
0xFCDE ARCTOP, INC.
0xFCDF NIO USA, Inc.
0xFCE0 Akciju sabiedriba ”SAF TEHNIKA”
0xFCE1 Sony Group Corporation
0xFCE2 Baracoda Daily Healthtech
0xFCE3 Smith & Nephew Medical Limited
0xFCE4 Samsara Networks, Inc
0xFCE5 Samsara Networks, Inc
0xFCE6 Guard RFID Solutions Inc.
0xFCE7 TKH Security B.V.
0xFCE8 ITT Industries
0xFCE9 MindRhythm, Inc.
0xFCEA Chess Wise B.V.
0xFCEB Avi-On
0xFCEC Griffwerk GmbH
0xFCED Workaround Gmbh
0xFCEE Velentium, LLC
0xFCEF Divesoft s.r.o.
0xFCF0 Security Enhancement Systems, LLC
0xFCF1 Google LLC
0xFCF2 Bitwards Oy
0xFCF3 Armatura LLC
0xFCF4 Allegion
0xFCF5 Trident Communication Technology, LLC
0xFCF6 The Linux Foundation
0xFCF7 Honor Device Co., Ltd.
0xFCF8 Honor Device Co., Ltd.
0xFCF9 Leupold & Stevens, Inc.
0xFCFA Leupold & Stevens, Inc.
0xFCFB Shenzhen Benwei Media Co., Ltd.
0xFCFC Barrot Technology Co.,Ltd.
0xFCFD Barrot Technology Co.,Ltd.
0xFCFE Sonova Consumer Hearing GmbH
0xFCFF 701x
0xFD00 FUTEK Advanced Sensor Technology, Inc.
0xFD01 Sanvita Medical Corporation
0xFD02 LEGO System A/S
0xFD03 Quuppa Oy
0xFD04 Shure Inc.
0xFD05 Qualcomm Technologies, Inc.
0xFD06 RACE-AI LLC
0xFD07 Swedlock AB
0xFD08 Bull Group Incorporated Company
0xFD09 Cousins and Sears LLC
0xFD0A Luminostics, Inc.
0xFD0B Luminostics, Inc.
0xFD0C OSM HK Limited
0xFD0D Blecon Ltd
0xFD0E HerdDogg, Inc
0xFD0F AEON MOTOR CO.,LTD.
0xFD10 AEON MOTOR CO.,LTD.
0xFD11 AEON MOTOR CO.,LTD.
0xFD12 AEON MOTOR CO.,LTD.
0xFD13 BRG Sports, Inc.
0xFD14 BRG Sports, Inc.
0xFD15 Panasonic Corporation
0xFD16 Sensitech, Inc.
0xFD17 LEGIC Identsystems AG
0xFD18 LEGIC Identsystems AG
0xFD19 Smith & Nephew Medical Limited
0xFD1A CSIRO
0xFD1B Helios Sports, Inc.
0xFD1C Brady Worldwide Inc.
0xFD1D Samsung Electronics Co., Ltd
0xFD1E Plume Design Inc.
0xFD1F 3M
0xFD20 GN Hearing A/S
0xFD21 Huawei Technologies Co., Ltd.
0xFD22 Huawei Technologies Co., Ltd.
0xFD23 DOM Sicherheitstechnik GmbH & Co. KG
0xFD24 GD Midea Air-Conditioning Equipment Co., Ltd.
0xFD25 GD Midea Air-Conditioning Equipment Co., Ltd.
0xFD26 Novo Nordisk A/S
0xFD27 Integrated Illumination Systems, Inc.
0xFD28 Julius Blum GmbH
0xFD29 Asahi Kasei Corporation
0xFD2A Sony Corporation
0xFD2B The Access Technologies
0xFD2C The Access Technologies
0xFD2D Xiaomi Inc.
0xFD2E Bitstrata Systems Inc.
0xFD2F Bitstrata Systems Inc.
0xFD30 Sesam Solutions BV
0xFD31 LG Electronics Inc.
0xFD32 Gemalto Holding BV
0xFD33 DashLogic, Inc.
0xFD34 Aerosens LLC.
0xFD35 Transsion Holdings Limited
0xFD36 Google LLC
0xFD37 TireCheck GmbH
0xFD38 Danfoss A/S
0xFD39 PREDIKTAS
0xFD3A Verkada Inc.
0xFD3B Verkada Inc.
0xFD3C Redline Communications Inc.
0xFD3D Woan Technology (Shenzhen) Co., Ltd.
0xFD3E Pure Watercraft, inc.
0xFD3F Cognosos, Inc
0xFD40 Beflex Inc.
0xFD41 Amazon Lab126
0xFD42 Globe (Jiangsu) Co.,Ltd
0xFD43 Apple Inc.
0xFD44 Apple Inc.
0xFD45 GB Solution co.,Ltd
0xFD46 Lemco IKE
0xFD47 Liberty Global Inc.
0xFD48 Geberit International AG
0xFD49 Panasonic Corporation
0xFD4A Sigma Elektro GmbH
0xFD4B Samsung Electronics Co., Ltd.
0xFD4C Adolf Wuerth GmbH & Co KG
0xFD4D 70mai Co.,Ltd.
0xFD4E 70mai Co.,Ltd.
0xFD4F SONITOR TECHNOLOGIES AS
0xFD50 Hangzhou Tuya Information Technology Co., Ltd
0xFD51 UTC Fire and Security
0xFD52 UTC Fire and Security
0xFD53 PCI Private Limited
0xFD54 Qingdao Haier Technology Co., Ltd.
0xFD55 Braveheart Wireless, Inc.
0xFD56 Resmed Ltd
0xFD57 Volvo Car Corporation
0xFD58 Volvo Car Corporation
0xFD59 Samsung Electronics Co., Ltd.
0xFD5A Samsung Electronics Co., Ltd.
0xFD5B V2SOFT INC.
0xFD5C React Mobile
0xFD5D maxon motor ltd.
0xFD5E Tapkey GmbH
0xFD5F Meta Platforms Technologies, LLC
0xFD60 Sercomm Corporation
0xFD61 Arendi AG
0xFD62 Google LLC
0xFD63 Google LLC
0xFD64 INRIA
0xFD65 Razer Inc.
0xFD66 Zebra Technologies Corporation
0xFD67 Montblanc Simplo GmbH
0xFD68 Ubique Innovation AG
0xFD69 Samsung Electronics Co., Ltd
0xFD6A Emerson
0xFD6B rapitag GmbH
0xFD6C Samsung Electronics Co., Ltd.
0xFD6D Sigma Elektro GmbH
0xFD6E Polidea sp. z o.o.
0xFD6F Apple, Inc.
0xFD70 GuangDong Oppo Mobile Telecommunications Corp., Ltd
0xFD71 GN Hearing A/S
0xFD72 Logitech International SA
0xFD73 BRControls Products BV
0xFD74 BRControls Products BV
0xFD75 Insulet Corporation
0xFD76 Insulet Corporation
0xFD77 Withings
0xFD78 Withings
0xFD79 Withings
0xFD7A Withings
0xFD7B WYZE LABS, INC.
0xFD7C Toshiba Information Systems(Japan) Corporation
0xFD7D Center for Advanced Research Wernher Von Braun
0xFD7E Samsung Electronics Co., Ltd.
0xFD7F Husqvarna AB
0xFD80 Phindex Technologies, Inc
0xFD81 CANDY HOUSE, Inc.
0xFD82 Sony Corporation
0xFD83 iNFORM Technology GmbH
0xFD84 Tile, Inc.
0xFD85 Husqvarna AB
0xFD86 Abbott
0xFD87 Google LLC
0xFD88 Urbanminded LTD
0xFD89 Urbanminded LTD
0xFD8A Signify Netherlands B.V.
0xFD8B Jigowatts Inc.
0xFD8C Google LLC
0xFD8D quip NYC Inc.
0xFD8E Motorola Solutions
0xFD90 Guangzhou SuperSound Information Technology Co.,Ltd
0xFD91 Groove X, Inc.
0xFD92 Qualcomm Technologies International, Ltd. (QTIL)
0xFD93 Bayerische Motoren Werke AG
0xFD94 Hewlett Packard Enterprise
0xFD95 Rigado
0xFD96 Google LLC
0xFD97 June Life, Inc.
0xFD98 Disney Worldwide Services, Inc.
0xFD99 ABB Oy
0xFD9A Huawei Technologies Co., Ltd.
0xFD9B Huawei Technologies Co., Ltd.
0xFD9C Huawei Technologies Co., Ltd.
0xFD9D Gastec Corporation
0xFD9E The Coca-Cola Company
0xFD9F VitalTech Affiliates LLC
0xFDA0 Secugen Corporation
0xFDA1 Groove X, Inc
0xFDA2 Groove X, Inc
0xFDA3 Inseego Corp.
0xFDA4 Inseego Corp.
0xFDA5 Neurostim OAB, Inc.
0xFDA6 WWZN Information Technology Company Limited
0xFDA7 WWZN Information Technology Company Limited
0xFDA8 PSA Peugeot Citroën
0xFDA9 Rhombus Systems, Inc.
0xFDAA Xiaomi Inc.
0xFDAB Xiaomi Inc.
0xFDAC Tentacle Sync GmbH
0xFDAD Houwa System Design, k.k.
0xFDAE Houwa System Design, k.k.
0xFDAF Wiliot LTD
0xFDB0 Oura Health Ltd
0xFDB1 Oura Health Ltd
0xFDB2 Portable Multimedia Ltd
0xFDB3 Audiodo AB
0xFDB4 HP Inc
0xFDB5 ECSG
0xFDB6 GWA Hygiene GmbH
0xFDB7 LivaNova USA Inc.
0xFDB8 LivaNova USA Inc.
0xFDBB Profoto
0xFDBC Emerson
0xFDBD Clover Network, Inc.
0xFDBE California Things Inc.
0xFDBF California Things Inc.
0xFDC0 Hunter Douglas
0xFDC1 Hunter Douglas
0xFDC2 Baidu Online Network Technology (Beijing) Co., Ltd
0xFDC3 Baidu Online Network Technology (Beijing) Co., Ltd
0xFDC4 Simavita (Aust) Pty Ltd
0xFDC5 Automatic Labs
0xFDC6 Eli Lilly and Company
0xFDC7 Eli Lilly and Company
0xFDC8 Hach – Danaher
0xFDC9 Busch-Jaeger Elektro GmbH
0xFDCA Fortin Electronic Systems
0xFDCB Meggitt SA
0xFDCC Shoof Technologies
0xFDCD Qingping Technology (Beijing) Co., Ltd.
0xFDCE SENNHEISER electronic GmbH & Co. KG
0xFDCF Nalu Medical, Inc
0xFDD0 Huawei Technologies Co., Ltd
0xFDD1 Huawei Technologies Co., Ltd
0xFDD2 Bose Corporation
0xFDD3 FUBA Automotive Electronics GmbH
0xFDD4 LX Solutions Pty Limited
0xFDD5 Brompton Bicycle Ltd
0xFDD6 Ministry of Supply
0xFDD7 Copeland Cold Chain LP
0xFDD8 Jiangsu Teranovo Tech Co., Ltd.
0xFDD9 Jiangsu Teranovo Tech Co., Ltd.
0xFDDA MHCS
0xFDDB Samsung Electronics Co., Ltd.
0xFDDC 4iiii Innovations Inc.
0xFDDD Arch Systems Inc
0xFDDE Noodle Technology Inc.
0xFDDF Harman International
0xFDE0 John Deere
0xFDE1 Fortin Electronic Systems
0xFDE2 Google LLC
0xFDE3 Abbott Diabetes Care
0xFDE4 JUUL Labs, Inc.
0xFDE5 SMK Corporation
0xFDE6 Intelletto Technologies Inc
0xFDE7 SECOM Co., LTD
0xFDE8 Robert Bosch GmbH
0xFDE9 Spacesaver Corporation
0xFDEA SeeScan, Inc
0xFDEB Syntronix Corporation
0xFDEC Mannkind Corporation
0xFDED Pole Star
0xFDEE Huawei Technologies Co., Ltd.
0xFDEF ART AND PROGRAM, INC.
0xFDF0 Google LLC
0xFDF1 LAMPLIGHT Co.,Ltd
0xFDF2 AMICCOM Electronics Corporation
0xFDF3 Amersports
0xFDF4 O. E. M. Controls, Inc.
0xFDF5 Milwaukee Electric Tools
0xFDF6 AIAIAI ApS
0xFDF7 HP Inc.
0xFDF8 Onvocal
0xFDF9 INIA
0xFDFA Tandem Diabetes Care
0xFDFB Tandem Diabetes Care
0xFDFC Optrel AG
0xFDFD RecursiveSoft Inc.
0xFDFE ADHERIUM(NZ) LIMITED
0xFDFF OSRAM GmbH
0xFE00 Amazon.com Services, Inc.
0xFE01 Duracell U.S. Operations Inc.
0xFE02 Robert Bosch GmbH
0xFE03 Amazon.com Services, Inc.
0xFE04 Motorola Solutions, Inc.
0xFE05 CORE Transport Technologies NZ Limited
0xFE06 Qualcomm Technologies, Inc.
0xFE07 Sonos, Inc.
0xFE08 Microsoft
0xFE09 Pillsy, Inc.
0xFE0A ruwido austria gmbh
0xFE0B ruwido austria gmbh
0xFE0C Procter & Gamble
0xFE0D Procter & Gamble
0xFE0E Setec Pty Ltd
0xFE0F Signify Netherlands B.V. (formerly Philips Lighting B.V.)
0xFE10 LAPIS Technology Co., Ltd.
0xFE11 GMC-I Messtechnik GmbH
0xFE12 M-Way Solutions GmbH
0xFE13 Apple Inc.
0xFE14 Flextronics International USA Inc.
0xFE15 Amazon.com Services, Inc..
0xFE16 Footmarks, Inc.
0xFE17 Telit Wireless Solutions GmbH
0xFE18 Runtime, Inc.
0xFE19 Google LLC
0xFE1A Tyto Life LLC
0xFE1B Tyto Life LLC
0xFE1C NetMedia, Inc.
0xFE1D Illuminati Instrument Corporation
0xFE1E LAMPLIGHT Co., Ltd.
0xFE1F Garmin International, Inc.
0xFE20 Emerson
0xFE21 Bose Corporation
0xFE22 Zoll Medical Corporation
0xFE23 Zoll Medical Corporation
0xFE24 August Home Inc
0xFE25 Apple, Inc.
0xFE26 Google LLC
0xFE27 Google LLC
0xFE28 Ayla Networks
0xFE29 Gibson Innovations
0xFE2A DaisyWorks, Inc.
0xFE2B ITT Industries
0xFE2C Google LLC
0xFE2D LAMPLIGHT Co., Ltd.
0xFE2E ERi,Inc.
0xFE2F CRESCO Wireless, Inc
0xFE30 Volkswagen AG
0xFE31 Volkswagen AG
0xFE32 Pro-Mark, Inc.
0xFE33 CHIPOLO d.o.o.
0xFE34 SmallLoop LLC
0xFE35 HUAWEI Technologies Co., Ltd
0xFE36 HUAWEI Technologies Co., Ltd
0xFE39 TTS Tooltechnic Systems AG & Co. KG
0xFE3A TTS Tooltechnic Systems AG & Co. KG
0xFE3B Dolby Laboratories
0xFE3C alibaba
0xFE3D BD Medical
0xFE3E BD Medical
0xFE3F Friday Labs Limited
0xFE40 Inugo Systems Limited
0xFE41 Inugo Systems Limited
0xFE42 Nets A/S
0xFE43 Andreas Stihl AG & Co. KG
0xFE44 SK Telecom
0xFE45 Snapchat Inc
0xFE46 B&O Play A/S
0xFE47 General Motors
0xFE48 General Motors
0xFE49 SenionLab AB
0xFE4A OMRON HEALTHCARE Co., Ltd.
0xFE4B Signify Netherlands B.V. (formerly Philips Lighting B.V.)
0xFE4C Volkswagen AG
0xFE4D Casambi Technologies Oy
0xFE4E NTT docomo
0xFE4F Molekule, Inc.
0xFE50 Google LLC
0xFE51 SRAM
0xFE52 SetPoint Medical
0xFE53 3M
0xFE54 Motiv, Inc.
0xFE55 Google LLC
0xFE56 Google LLC
0xFE57 Dotted Labs
0xFE58 Nordic Semiconductor ASA
0xFE59 Nordic Semiconductor ASA
0xFE5A Cronologics Corporation
0xFE5B GT-tronics HK Ltd
0xFE5C million hunters GmbH
0xFE5D Grundfos A/S
0xFE5E Plastc Corporation
0xFE5F Eyefi, Inc.
0xFE60 Lierda Science & Technology Group Co., Ltd.
0xFE61 Logitech International SA
0xFE62 Indagem Tech LLC
0xFE63 Connected Yard, Inc.
0xFE64 Siemens AG
0xFE65 CHIPOLO d.o.o.
0xFE66 Intel Corporation
0xFE67 Lab Sensor Solutions
0xFE68 Capsle Technologies Inc.
0xFE69 Capsle Technologies Inc.
0xFE6A Kontakt Micro-Location Sp. z o.o.
0xFE6B TASER International, Inc.
0xFE6C TASER International, Inc.
0xFE6D The University of Tokyo
0xFE6E The University of Tokyo
0xFE6F LINE Corporation
0xFE70 Beijing Jingdong Century Trading Co., Ltd.
0xFE71 Plume Design Inc
0xFE72 Abbott (formerly St. Jude Medical, Inc.)
0xFE73 Abbott (formerly St. Jude Medical, Inc.)
0xFE74 unwire
0xFE75 TangoMe
0xFE76 TangoMe
0xFE77 Hewlett-Packard Company
0xFE78 Hewlett-Packard Company
0xFE79 Zebra Technologies
0xFE7A Bragi GmbH
0xFE7B Orion Labs, Inc.
0xFE7C Telit Wireless Solutions (Formerly Stollmann E+V GmbH)
0xFE7D Aterica Health Inc.
0xFE7E Awear Solutions Ltd
0xFE7F Doppler Lab
0xFE80 Doppler Lab
0xFE81 Medtronic Inc.
0xFE82 Medtronic Inc.
0xFE83 Blue Bite
0xFE84 RF Digital Corp
0xFE85 RF Digital Corp
0xFE86 HUAWEI Technologies Co., Ltd
0xFE87 Qingdao Yeelink Information Technology Co., Ltd. (
青岛亿联客信息技术有限公司 )
0xFE88 SALTO SYSTEMS S.L.
0xFE89 B&O Play A/S
0xFE8A Apple, Inc.
0xFE8B Apple, Inc.
0xFE8C TRON Forum
0xFE8D Interaxon Inc.
0xFE8E ARM Ltd
0xFE8F CSR
0xFE90 JUMA
0xFE91 Shanghai Imilab Technology Co.,Ltd
0xFE92 Jarden Safety & Security
0xFE93 OttoQ In
0xFE94 OttoQ In
0xFE95 Xiaomi Inc.
0xFE96 Tesla Motors Inc.
0xFE97 Tesla Motors Inc.
0xFE98 Currant Inc
0xFE99 Currant Inc
0xFE9A Estimote
0xFE9B Samsara Networks, Inc
0xFE9C GSI Laboratories, Inc.
0xFE9D Mobiquity Networks Inc
0xFE9E Renesas Design Netherlands B.V.
0xFE9F Google LLC
0xFEA0 Google LLC
0xFEA3 ITT Industries
0xFEA4 Paxton Access Ltd
0xFEA5 GoPro, Inc.
0xFEA6 GoPro, Inc.
0xFEA7 UTC Fire and Security
0xFEA8 Savant Systems LLC
0xFEA9 Savant Systems LLC
0xFEAA Google LLC
0xFEAB Nokia
0xFEAC Nokia
0xFEAD Nokia
0xFEAE Nokia
0xFEAF Nest Labs Inc
0xFEB0 Nest Labs Inc
0xFEB1 Electronics Tomorrow Limited
0xFEB2 Microsoft Corporation
0xFEB3 Taobao
0xFEB4 WiSilica Inc.
0xFEB5 WiSilica Inc.
0xFEB6 Vencer Co., Ltd
0xFEB7 Meta Platforms, Inc.
0xFEB8 Meta Platforms, Inc.
0xFEB9 LG Electronics
0xFEBA Tencent Holdings Limited
0xFEBB adafruit industries
0xFEBC Dexcom Inc
0xFEBD Clover Network, Inc
0xFEBE Bose Corporation
0xFEBF Nod, Inc.
0xFEC0 KDDI Corporation
0xFEC1 KDDI Corporation
0xFEC2 Blue Spark Technologies, Inc.
0xFEC3 360fly, Inc.
0xFEC4 PLUS Location Systems
0xFEC5 Realtek Semiconductor Corp.
0xFEC6 Kocomojo, LLC
0xFEC7 Apple, Inc.
0xFEC8 Apple, Inc.
0xFEC9 Apple, Inc.
0xFECA Apple, Inc.
0xFECB Apple, Inc.
0xFECC Apple, Inc.
0xFECD Apple, Inc.
0xFECE Apple, Inc.
0xFECF Apple, Inc.
0xFED0 Apple, Inc.
0xFED1 Apple, Inc.
0xFED2 Apple, Inc.
0xFED3 Apple, Inc.
0xFED4 Apple, Inc.
0xFED5 Plantronics Inc.
0xFED6 Broadcom
0xFED7 Broadcom
0xFED8 Google LLC
0xFED9 Pebble Technology Corporation
0xFEDA ISSC Technologies Corp.
0xFEDB Perka, Inc.
0xFEDC Jawbone
0xFEDD Jawbone
0xFEDE Coin, Inc.
0xFEE0 Anhui Huami Information Technology Co., Ltd.
0xFEE1 Anhui Huami Information Technology Co., Ltd.
0xFEE2 Anki, Inc.
0xFEE3 Anki, Inc.
0xFEE4 Nordic Semiconductor ASA
0xFEE5 Nordic Semiconductor ASA
0xFEE6 Silvair, Inc.
0xFEE7 Tencent Holdings Limited.
0xFEE8 Quintic Corp.
0xFEE9 Quintic Corp.
0xFEEA Swirl Networks, Inc.
0xFEEB Swirl Networks, Inc.
0xFEEC Tile, Inc.
0xFEED Tile, Inc.
0xFEEE Polar Electro Oy
0xFEEF Polar Electro Oy
0xFEF0 Intel
0xFEF1 CSR
0xFEF2 CSR
0xFEF3 Google LLC
0xFEF4 Google LLC
0xFEF5 Dialog Semiconductor GmbH
0xFEF6 Wicentric, Inc.
0xFEF7 Aplix Corporation
0xFEF8 Aplix Corporation
0xFEF9 PayPal, Inc.
0xFEFA PayPal, Inc.
0xFEFB Telit Wireless Solutions (Formerly Stollmann E+V GmbH)
0xFEFC Gimbal, Inc.
0xFEFD Gimbal, Inc.
0xFEFE GN Hearing A/S
0xFEFF GN Netcom
""".strip()


def _build_member_service_uuid_descriptions() -> dict[int, str]:
    member_uuids = _parse_uuid_name_table(_MEMBER_SERVICE_UUIDS_TABLE)
    return {uuid16: f"Member Service UUID: {company}" for uuid16, company in member_uuids.items()}


# Well-known overrides (same UUID key, more specific description).
_KNOWN_SERVICE_UUID_OVERRIDES: Final[dict[int, str]] = {
    0xFE2C: "Google Fast Pair",
    0xFEAA: "Google Eddystone",
    # Apple Find My Network (FMN) accessory services (often seen via AD type 0x16 Service Data).
    0xFD44: "Apple Find My network service (FMN)",
    0xFD43: "Apple Find My firmware update service (FMN)",
    0xFD6F: "Exposure Notification (GAEN / contact tracing)",
    0xFCB2: "DULT (Detecting Unwanted Location Trackers)",
    # Common tracker UUIDs seen in the wild.
    0xFEEC: "Tile (tracker) — Member Service UUID",
    0xFEED: "Tile (tracker) — Member Service UUID",
    0xFD84: "Tile (tracker) — Member Service UUID",
    0xFD59: "Samsung SmartTag — Member Service UUID",
    0xFD5A: "Samsung SmartTag — Member Service UUID",
    0xFD69: "Samsung SmartTag — Member Service UUID",
    # Chipolo trackers often advertise one of these member UUIDs.
    0xFE33: "Chipolo (tracker) — Member Service UUID",
    0xFE65: "Chipolo (tracker) — Member Service UUID",
}


SERVICE_UUIDS: Final[dict[int, str]] = {
    **_GATT_SERVICE_UUIDS_BY_UUID,
    **_SDO_SERVICE_UUIDS,
    # Member services (company-only naming)
    **_build_member_service_uuid_descriptions(),
    # Overrides
    **_KNOWN_SERVICE_UUID_OVERRIDES,
}


# Curated Bluetooth SIG Company Identifiers (Manufacturer Specific Data).
# NOTE: Some consumer brands do not have their own assigned Company Identifier and may advertise
# under an ODM/OEM or chipset vendor instead.
COMPANY_IDS: Final[dict[int, str]] = {
    # Big tech / consumer electronics
    0x004C: "Apple, Inc.",
    0x0075: "Samsung Electronics Co. Ltd.",
    0x00E0: "Google",
    0x018E: "Google LLC",
    0x0006: "Microsoft",
    0x0171: "Amazon.com Services LLC",
    0x012D: "Sony Corporation",
    0x009E: "Bose Corporation",
    0x0057: "Harman International Industries, Inc. (JBL)",
    0x0494: "SENNHEISER electronic GmbH & Co. KG",
    0x0067: "GN Hearing",
    0x0089: "GN Hearing A/S",
    0x0103: "Bang & Olufsen A/S",
    0x00CC: "Beats Electronics",
    0x0CC2: "Anker Innovations Limited",
    0x07C9: "Skullcandy, Inc.",
    0x05A7: "Sonos Inc",

    # Wearables / fitness
    0x0087: "Garmin International, Inc.",
    0x006B: "Polar Electro OY",
    0x00D1: "Polar Electro Europe B.V.",
    0x009F: "Suunto Oy",
    0x02B2: "Oura Health Oy",
    0x0304: "Oura Health Ltd",
    0x0157: "Anhui Huami Information Technology Co., Ltd. (Amazfit/Zepp)",
    0x03FF: "Withings",

    # Trackers
    0x067C: "Tile, Inc.",
    0x08C3: "CHIPOLO d.o.o.",

    # Vehicles
    0x022B: "Tesla, Inc.",
    0x05EB: "Bayerische Motoren Werke AG (BMW)",
    0x017C: "Mercedes-Benz Group AG",
    0x0723: "Ford Motor Company",
    0x0068: "General Motors",
    0x0977: "TOYOTA motor corporation",
    0x0915: "Honda Motor Co., Ltd.",
    0x011F: "Volkswagen AG",
    0x0941: "Rivian Automotive, LLC",

    # Phones / OEMs
    0x038F: "Xiaomi Inc.",
    0x027D: "HUAWEI Technologies Co., Ltd.",
    0x072F: "OnePlus Electronics (Shenzhen) Co., Ltd.",
    0x079A: "GuangDong Oppo Mobile Telecommunications Corp., Ltd.",
    0x08A4: "Realme Chongqing Mobile Telecommunications Corp., Ltd.",
    0x0837: "vivo Mobile Communication Co., Ltd.",
    0x0CCB: "NOTHING TECHNOLOGY LIMITED",

    # Drones / robotics
    0x08AA: "SZ DJI TECHNOLOGY CO.,LTD",
    0x0043: "PARROT AUTOMOTIVE SAS",

    # Cameras / networking
    0x0E25: "Hangzhou Hikvision Digital Technology Co., Ltd.",
    0x0D5B: "Axis Communications AB",

    # Smart home / IoT ecosystems
    0x01DD: "Koninklijke Philips N.V.",
    0x060F: "Signify Netherlands B.V.",
    0x07D0: "Hangzhou Tuya Information Technology Co., Ltd.",
    0x06D0: "Etekcity Corporation (Govee)",
    0x0969: "Woan Technology (Shenzhen) Co., Ltd. (SwitchBot)",
    0x080B: "Nanoleaf Canada Limited",
    0x07D6: "ecobee Inc.",
    0x01D1: "August Home, Inc",
    0x0BDE: "Yale",

    # Gaming
    0x0553: "Nintendo Co., Ltd.",
    0x055D: "Valve Corporation",
}


# Curated Fast Pair Model IDs (24-bit) → device name.
# Source: community-discovered lists (not an official SIG assigned-number list).
FAST_PAIR_MODELS: Final[dict[int, str]] = {
    # Google / Pixel
    0x060000: "Google Pixel Buds",
    0x0582FD: "Pixel Buds",
    0x92BBBD: "Pixel Buds",

    # Bose
    0xF00000: "Bose QuietComfort 35 II",
    0x0100F0: "Bose QuietComfort 35 II",
    0xCD8256: "Bose NC 700",

    # Sony
    0x01EEB4: "Sony WH-1000XM4",
    0x058D08: "Sony WH-1000XM4",
    0xD446A7: "Sony XM5",
    0x2D7A23: "Sony WF-1000XM4",
    0x07A41C: "Sony WF-C700N",
    0x00C95C: "Sony WF-1000X",

    # Samsung
    0x0577B1: "Samsung Galaxy S23 Ultra",
    0x05A9BC: "Samsung Galaxy S20+",
    0x06AE20: "Samsung Galaxy S21 5G",

    # Jabra / GN
    0x00AA48: "Jabra Elite 2",

    # Bang & Olufsen
    0x00AA91: "B&O Beoplay E8 2.0",
    0x01AA91: "B&O Beoplay H9 3rd Generation",
    0x05AA91: "B&O Beoplay E6",
    0x04AA91: "B&O Beoplay H4",

    # Anker / Soundcore
    0x008F7D: "soundcore Glow Mini",
    0x06D8FC: "soundcore Liberty 4 NC",
    0x72FB00: "Soundcore Spirit Pro GVA",

    # Razer
    0x72EF8D: "Razer Hammerhead TWS X",
    0x0E30C3: "Razer Hammerhead TWS",

    # Nest / Google
    0x07F426: "Nest Hub Max",

    # Wearables
    0x057802: "TicWatch Pro 5",

    # JBL (selection)
    0xF00200: "JBL Everest 110GA",
    0xF00201: "JBL Everest 110GA",
    0xF00202: "JBL Everest 110GA",
    0xF00209: "JBL LIVE400BT",
    0xF0020E: "JBL LIVE500BT",
    0xF00213: "JBL LIVE650BTNC",
    0x02DD4F: "JBL TUNE770NC",
    0x02F637: "JBL LIVE FLEX",
    0x038CC7: "JBL TUNE760NC",
    0x04AFB8: "JBL TUNE 720BT",
    0x054B2D: "JBL TUNE125TWS",
    0x0660D7: "JBL LIVE770NC",
    0x821F66: "JBL Flip 6",
    0xF52494: "JBL Buds Pro",
    0x718FA4: "JBL Live 300TWS",

    # LG (legacy)
    0x001000: "LG HBS1110",
    0x002000: "AIAIAI TMA-2 (H60)",
    0x003000: "Libratone Q Adapt On-Ear",
    0x003001: "Libratone Q Adapt On-Ear",
    0x003B41: "M&D MW65",
    0x003D8A: "Cleer FLOW II",
    0xF00300: "LG HBS-835S",
    0x0003F0: "LG HBS-835S",

    # Misc popular audio
    0x00A168: "boAt Airdopes 621",
    0x00FA72: "Pioneer SE-MS9BN",
    0x011242: "Nirvana Ion",
    0x013D8A: "Cleer EDGE Voice",
    0x038B91: "DENON AH-C830NCW",
    0x038F16: "Beats Studio Buds",
    0x03C99C: "MOTO BUDS 135",
    0x04C95C: "Sony WI-1000X",
    0x050F0C: "Major III Voice",
    0x052CC7: "MINOR III",
    0x05A963: "WONDERBOOM 3",
    0x06C197: "OPPO Enco Air3 Pro",
    0x0744B6: "Technics EAH-AZ60M2",
    0x005BC3: "Panasonic RP-HD610N",
}


def ble_random_address_subtype_bits(mac: str) -> int | None:
    """Return the two MSBs (bits 47:46) of the address as an int 0..3.

    This is ONLY meaningful if you already know the address is a random BLE address.
    """
    if not mac:
        return None
    first = mac.split(":", 1)[0].strip()
    if len(first) != 2:
        return None
    try:
        b0 = int(first, 16)
    except ValueError:
        return None
    return (b0 & 0xC0) >> 6


def ble_random_address_subtype(
    mac: str,
) -> Literal["non_resolvable", "resolvable", "static", "reserved"] | None:
    """Classify random-address subtype from MAC bits 47:46.

    - 0b00: non-resolvable private
    - 0b01: resolvable private
    - 0b10: reserved
    - 0b11: static random
    """
    bits = ble_random_address_subtype_bits(mac)
    if bits is None:
        return None
    return {
        0b00: "non_resolvable",
        0b01: "resolvable",
        0b10: "reserved",
        0b11: "static",
    }[bits]


def looks_like_random_ble_address(mac: str) -> bool:
    """Heuristic-only check for random vs public from the address itself.

    Prefer OS/scan metadata (TxAdd / address type) when available.
    """
    bits = ble_random_address_subtype_bits(mac)
    if bits is None:
        return False
    # Public addresses can also have any upper bits, so this can be wrong.
    # In practice, many random addresses fall into 00/01/11.
    return bits in (0b00, 0b01, 0b11)
