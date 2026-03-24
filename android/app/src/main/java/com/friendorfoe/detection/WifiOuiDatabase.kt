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
    private val OUI_MAP: Map<String, OuiEntry> = mapOf(
        // DJI Technology — primary chipsets across all product lines
        "60:60:1F" to OuiEntry("DJI", "DJI Technology Co."),
        "34:D2:62" to OuiEntry("DJI", "DJI Technology Co."),
        "48:1C:B9" to OuiEntry("DJI", "DJI Innovation Technology"),
        "08:D4:6A" to OuiEntry("DJI", "DJI Technology (Shenzhen)"),
        "D0:32:9A" to OuiEntry("DJI", "DJI Technology Co."),
        "C4:2F:90" to OuiEntry("DJI", "DJI Technology Co."),
        "04:A8:5A" to OuiEntry("DJI", "DJI Technology Co."),          // Older Phantom/Inspire
        "0C:9A:E6" to OuiEntry("DJI", "DJI Technology Co."),          // Mavic/Mini series
        "E4:7A:2C" to OuiEntry("DJI", "DJI Technology Co."),          // OcuSync modules
        "58:B8:58" to OuiEntry("DJI", "DJI Technology Co."),          // FPV/Avata
        "8C:58:23" to OuiEntry("DJI", "DJI Technology Co."),          // Newer models
        "88:29:85" to OuiEntry("DJI", "DJI Technology Co."),          // Enterprise/Matrice
        "4C:43:F6" to OuiEntry("DJI", "DJI Technology Co."),          // Agras/agricultural

        // Parrot SA — Anafi, Bebop, Disco
        "A0:14:3D" to OuiEntry("Parrot", "Parrot SA"),
        "90:03:B7" to OuiEntry("Parrot", "Parrot SA"),
        "00:12:1C" to OuiEntry("Parrot", "Parrot SA"),
        "00:26:7E" to OuiEntry("Parrot", "Parrot SA"),

        // Autel Robotics — EVO, EVO Max, EVO Nano/Lite
        "2C:DC:AD" to OuiEntry("Autel", "Autel Robotics"),
        // NOTE: 78:8C:B5 was previously listed as Autel but is actually TP-Link (removed)
        "EC:5B:CD" to OuiEntry("Autel", "Autel Robotics USA LLC"),    // Registered May 2024
        "18:D7:93" to OuiEntry("Autel", "Autel Intelligent Technology"), // Registered Jan 2023

        // Skydio — S2, X2, X10
        "58:D5:6E" to OuiEntry("Skydio", "Skydio Inc."),
        "38:1D:14" to OuiEntry("Skydio", "Skydio Inc."),              // Registered July 2019

        // Yuneec — Typhoon, H520
        "EC:D0:9F" to OuiEntry("Yuneec", "Yuneec International"),
        "64:D4:DA" to OuiEntry("Yuneec", "Yuneec International"),

        // Zero Zero Robotics — HOVERAir X1
        "10:D0:7A" to OuiEntry("HOVERAir", "Zero Zero Robotics"),

        // Xiaomi / FIMI — FIMI X8, Xiaomi Mi Drone
        "28:6C:07" to OuiEntry("Xiaomi", "Xiaomi Communications"),
        "64:CE:01" to OuiEntry("Xiaomi", "Xiaomi Communications"),
        "9C:99:A0" to OuiEntry("FIMI", "Xiaomi FIMI"),

        // Hubsan — Zino series
        "D8:96:E0" to OuiEntry("Hubsan", "Hubsan Technology"),

        // Holy Stone — HS series
        "CC:DB:A7" to OuiEntry("Holy Stone", "Holy Stone"),

        // Potensic — Dreamer series
        "B0:A7:32" to OuiEntry("Potensic", "Potensic"),

        // Walkera — Vitus, Runner series
        "C8:14:51" to OuiEntry("Walkera", "Walkera Technology Co."),

        // Syma — X-series toy drones
        "E8:AB:FA" to OuiEntry("Syma", "Syma"),

        // Espressif Systems — ESP32/ESP8266 used in many DIY/budget drones
        "24:0A:C4" to OuiEntry("Generic/ESP", "Espressif Systems", highFalsePositiveRisk = true),
        "30:AE:A4" to OuiEntry("Generic/ESP", "Espressif Systems", highFalsePositiveRisk = true),
        "A4:CF:12" to OuiEntry("Generic/ESP", "Espressif Systems", highFalsePositiveRisk = true),
        "AC:67:B2" to OuiEntry("Generic/ESP", "Espressif Systems", highFalsePositiveRisk = true),
        "10:06:1C" to OuiEntry("Generic/ESP", "Espressif Systems", highFalsePositiveRisk = true),

        // Realtek — used in many budget Chinese drones (WiFi FPV cameras)
        "00:E0:4C" to OuiEntry("Generic/Realtek", "Realtek Semiconductor", highFalsePositiveRisk = true),

        // Shenzhen Bilian (LB-LINK) — very common WiFi module in budget drones
        "08:EA:40" to OuiEntry("Generic/Bilian", "Shenzhen Bilian Electronic (LB-LINK)", highFalsePositiveRisk = true),

        // Hubsan — additional OUI
        "98:AA:FC" to OuiEntry("Hubsan", "Hubsan Technology"),

        // Yuneec — additional OUI
        "00:E0:6D" to OuiEntry("Yuneec", "Yuneec International"),

        // Military/defense drone manufacturers
        "14:DD:48" to OuiEntry("Shield AI", "Shield AI Inc."),         // Nova 1/2
        "14:DD:9C" to OuiEntry("Shield AI", "Shield AI Inc."),
        "00:1A:F9" to OuiEntry("AeroVironment", "AeroVironment Inc."), // Switchblade/Puma/Raven

        // Industrial drone datalink vendors
        "1C:BA:8C" to OuiEntry("UAV Navigation", "UAV Navigation S.L."),
        "00:30:1A" to OuiEntry("Doodle Labs", "Doodle Labs"),          // Long-range mesh
        "00:11:1C" to OuiEntry("Microhard", "Microhard Systems"),      // Military datalinks

        // Thermal camera payloads (found on drone platforms)
        "00:13:56" to OuiEntry("FLIR", "Teledyne FLIR"),

        // DJI adjacent entity
        "9C:5A:8A" to OuiEntry("DJI", "DJI Baiwang Technology Co Ltd"),

        // PowerVision (PowerEgg, PowerRay)
        "54:7D:40" to OuiEntry("PowerVision", "Powervision Tech Inc."),

        // Silvus Technologies (military MANET radios on AeroVironment drones)
        "C4:7C:8D" to OuiEntry("Silvus", "Silvus Technologies"),
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
     * Handles both colon and hyphen separators, and no separator.
     *
     * @param bssid Full MAC address string
     * @return Uppercase colon-separated OUI (e.g., "60:60:1F") or null if invalid
     */
    private fun extractOui(bssid: String): String? {
        val cleaned = bssid.uppercase().replace("-", ":")
        val parts = cleaned.split(":")
        if (parts.size >= 3) {
            return "${parts[0]}:${parts[1]}:${parts[2]}"
        }
        // Handle compact format (no separator): "60601F..."
        val hex = cleaned.replace(":", "")
        if (hex.length >= 6) {
            return "${hex.substring(0, 2)}:${hex.substring(2, 4)}:${hex.substring(4, 6)}"
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
