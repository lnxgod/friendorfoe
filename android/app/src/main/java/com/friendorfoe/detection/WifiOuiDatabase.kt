package com.friendorfoe.detection

/**
 * Lookup table mapping WiFi MAC address OUI prefixes (first 3 bytes) to
 * known drone hardware manufacturers.
 *
 * The OUI (Organizationally Unique Identifier) is assigned by IEEE and
 * identifies the chipset/device vendor. Even drones with hidden or generic
 * SSIDs still broadcast their hardware OUI in the BSSID.
 *
 * Sources: IEEE OUI registry, FCC filings, community hardware teardowns.
 */
object WifiOuiDatabase {

    /**
     * OUI prefix (uppercase, colon-separated) to manufacturer name.
     * Entries are organized by manufacturer for readability.
     */
    // All entries verified against the IEEE MA-L/MA-M/MA-S registry (2026-07).
    // Only exclusive /24 (MA-L) blocks are confident drone OUIs. MA-M(/28) and
    // MA-S(/36) blocks share their first 3 bytes across ~15 unrelated companies,
    // so — because lookup matches only the first 3 bytes — vendors that only hold
    // /28 registrations (Autel, Yuneec, Hubsan, Quantum-Systems, Silvus) are
    // detected via SSID/RID instead of OUI. Several legacy rows were mislabeled
    // and fired false drone alerts; they are corrected to the true IEEE owner and
    // flagged highFalsePositiveRisk. Kept in parity with esp32 wifi_oui_database.c.
    private val OUI_MAP: Map<String, OuiEntry> = mapOf(
        // DJI Technology (SZ DJI, Ronin, Osmo, Baiwang subsidiaries)
        "60:60:1F" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "34:D2:62" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "48:1C:B9" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "04:A8:5A" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "0C:9A:E6" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "4C:43:F6" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "58:B8:58" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "88:29:85" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "8C:58:23" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "E4:7A:2C" to OuiEntry("DJI", "SZ DJI Technology Co."),
        "F8:40:68" to OuiEntry("DJI", "SZ DJI Ronin Technology Co."),
        "20:1F:55" to OuiEntry("DJI", "DJI Osmo Technology Co."),
        "9C:5A:8A" to OuiEntry("DJI", "DJI Baiwang Technology Co."),
        "EC:72:F7" to OuiEntry("DJI", "DJI Baiwang Technology Co."),

        // Parrot SA — Anafi, Bebop, Disco
        "A0:14:3D" to OuiEntry("Parrot", "Parrot SA"),
        "90:03:B7" to OuiEntry("Parrot", "Parrot SA"),
        "00:12:1C" to OuiEntry("Parrot", "Parrot SA"),
        "00:26:7E" to OuiEntry("Parrot", "Parrot SA"),
        "90:3A:E6" to OuiEntry("Parrot", "Parrot SA"),

        // Skydio — S2, X2, X10 (58:D5:6E was mislabeled: it is D-Link)
        "38:1D:14" to OuiEntry("Skydio", "Skydio Inc."),

        // Zero Zero Robotics — HOVERAir X1
        "84:83:19" to OuiEntry("HOVERAir", "Hangzhou Zero Zero Technology"),

        // Teal Drones (Red Cat)
        "B0:30:C8" to OuiEntry("Teal", "Teal Drones, Inc."),

        // Freefly Systems
        "EC:71:5E" to OuiEntry("Freefly", "Freefly Systems Inc."),

        // PowerVision (PowerEgg, PowerRay)
        "54:7D:40" to OuiEntry("PowerVision", "Powervision Tech Inc."),

        // Military / tactical UAS + MANET datalinks (verified /24)
        "14:DD:48" to OuiEntry("Shield AI", "Shield AI Inc."),
        "00:1A:F9" to OuiEntry("AeroVironment", "AeroVironment Inc."), // Switchblade/Puma/Raven
        "00:18:A6" to OuiEntry("Persistent", "Persistent Systems LLC (MPU5)"),
        "00:0F:92" to OuiEntry("Microhard", "Microhard Systems (Canada)"),
        "00:1E:3F" to OuiEntry("TrellisWare", "TrellisWare Technologies"),
        "98:49:9F" to OuiEntry("DTC", "Domo Tactical Communications"),
        "00:30:1A" to OuiEntry("Doodle Labs", "Doodle Labs (Smartbridges)"),
        "00:13:56" to OuiEntry("FLIR", "Teledyne FLIR"),

        // Privacy infrastructure / ALPR / surveillance cameras
        "B4:1E:52" to OuiEntry("Flock Safety", "Flock Safety ALPR/camera registered OUI"),
        "C4:2F:90" to OuiEntry("Hikvision", "Hangzhou Hikvision (IP camera)", highFalsePositiveRisk = true),
        "E8:AB:FA" to OuiEntry("Reecam", "Shenzhen Reecam (IP camera)", highFalsePositiveRisk = true),

        // Generic module makers (seen on budget drones AND unrelated IoT).
        // B0:A7:32/CC:DB:A7 were mislabeled Potensic/Holy Stone (both Espressif);
        // 10:D0:7A was HOVERAir (AMPAK), 2C:DC:AD was Autel (WNC),
        // EC:D0:9F was Yuneec (Xiaomi), 78:8C:B5 was Autel (TP-Link).
        "24:0A:C4" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "30:AE:A4" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "A4:CF:12" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "AC:67:B2" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "10:06:1C" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "B0:A7:32" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "CC:DB:A7" to OuiEntry("Espressif", "Espressif Systems", highFalsePositiveRisk = true),
        "00:E0:4C" to OuiEntry("Realtek", "Realtek Semiconductor", highFalsePositiveRisk = true),
        "08:EA:40" to OuiEntry("Bilian", "Shenzhen Bilian (LB-LINK)", highFalsePositiveRisk = true),
        "10:D0:7A" to OuiEntry("AMPAK", "AMPAK Technology (WiFi module)", highFalsePositiveRisk = true),
        "2C:DC:AD" to OuiEntry("WNC", "Wistron NeWeb (ODM module)", highFalsePositiveRisk = true),
        "28:6C:07" to OuiEntry("Xiaomi", "Xiaomi Communications", highFalsePositiveRisk = true),
        "EC:D0:9F" to OuiEntry("Xiaomi", "Xiaomi Communications", highFalsePositiveRisk = true),
        "9C:99:A0" to OuiEntry("Xiaomi/FIMI", "Xiaomi Communications (FIMI)", highFalsePositiveRisk = true),
        "78:8C:B5" to OuiEntry("TP-Link", "TP-Link Systems", highFalsePositiveRisk = true),

        // Removed as mislabels (true owner is an unrelated consumer vendor, no
        // detection value): 08:D4:6A=LG, 64:D4:DA=Intel, 58:D5:6E=D-Link,
        // C8:14:51=Huawei, D8:96:E0=Alibaba Cloud, 00:E0:6D=Compuware,
        // 14:DD:9C=vivo, 1C:BA:8C=Texas Instruments, 00:11:1C=Pleora; observed-only
        // (not in IEEE): D0:32:9A, 64:CE:01; shared /28 blocks dropped:
        // EC:5B:CD/18:D7:93 (Autel), 98:AA:FC (Hubsan), C4:7C:8D (Silvus).
        // Holy Stone 00:0C:BF NOT added: that IEEE record is a capacitor maker.
    )

    /**
     * Look up the manufacturer by BSSID (MAC address).
     *
     * @param bssid Full MAC address in format "AA:BB:CC:DD:EE:FF"
     * @return [OuiEntry] if the OUI matches a known drone vendor, null otherwise
     */
    fun lookup(bssid: String): OuiEntry? {
        val oui = extractOui(bssid) ?: return null
        return OUI_MAP[oui]
    }

    /**
     * Check if a BSSID belongs to a known drone manufacturer.
     *
     * @param bssid Full MAC address
     * @return true if OUI matches a known drone vendor (excluding high-false-positive entries)
     */
    fun isDroneOui(bssid: String): Boolean {
        val entry = lookup(bssid) ?: return false
        return !entry.highFalsePositiveRisk
    }

    /**
     * Extract the OUI prefix (first 3 octets) from a MAC address.
     * Handles colon, hyphen, dot, compact, and harmless outer whitespace.
     *
     * @param bssid Full MAC address string
     * @return Uppercase colon-separated OUI (e.g., "60:60:1F") or null if invalid
     */
    private fun extractOui(bssid: String): String? {
        val hexChars = mutableListOf<Char>()
        for (ch in bssid.trim().uppercase()) {
            if (ch == ':' || ch == '-' || ch == '.') {
                if (hexChars.isEmpty()) return null
                continue
            }
            if (ch !in '0'..'9' && ch !in 'A'..'F') return null
            hexChars += ch
            if (hexChars.size == 6) {
                val hex = hexChars.joinToString("")
                return "${hex.substring(0, 2)}:${hex.substring(2, 4)}:${hex.substring(4, 6)}"
            }
        }
        return null
    }
}

/**
 * Entry in the OUI database for a known drone hardware vendor.
 *
 * @property manufacturer Short manufacturer name for display (e.g., "DJI")
 * @property fullName Full registered organization name
 * @property highFalsePositiveRisk True if this OUI is also commonly used in non-drone devices
 */
data class OuiEntry(
    val manufacturer: String,
    val fullName: String,
    val highFalsePositiveRisk: Boolean = false
)
