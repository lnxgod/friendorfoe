package com.friendorfoe.detection

import java.util.Locale

/** Validation and stable IDs for the Wi-Fi radio Android actually observed. */
internal object WifiTransmitterIdentity {
    private val canonicalPattern = Regex("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
    private val rejectedAddresses = setOf(
        "00:00:00:00:00:00",
        "02:00:00:00:00:00",
        "FF:FF:FF:FF:FF:FF",
    )

    fun normalize(raw: String?): String? {
        val canonical = raw
            ?.trim()
            ?.replace('-', ':')
            ?.uppercase(Locale.ROOT)
            ?: return null
        if (!canonicalPattern.matches(canonical) || canonical in rejectedAddresses) return null

        val firstOctet = canonical.substring(0, 2).toInt(16)
        if ((firstOctet and 0x01) != 0) return null
        return canonical
    }

    fun identityKey(ssid: String, normalizedBssid: String?): String =
        normalizedBssid ?: "ssid:${ssidSlug(ssid)}"

    fun detectionId(prefix: String, ssid: String, normalizedBssid: String?): String {
        val suffix = normalizedBssid
            ?.replace(":", "")
            ?.lowercase(Locale.ROOT)
            ?: ssidSlug(ssid)
        return "${prefix}_$suffix"
    }

    private fun ssidSlug(ssid: String): String = ssid
        .lowercase(Locale.ROOT)
        .replace(Regex("[^a-z0-9]"), "_")
        .ifBlank { "unknown" }
}
