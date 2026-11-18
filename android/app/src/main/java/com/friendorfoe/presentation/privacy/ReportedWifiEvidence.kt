package com.friendorfoe.presentation.privacy

/** Radio source codes published by the badge and backend, independent of the transport to the phone. */
internal fun reportedWifiRule(source: String?): String? = when (source) {
    "wifi_ssid" -> "The scanner matched a Wi-Fi network name to a device signature."
    "wifi_oui" -> "The scanner matched a Wi-Fi address prefix to a vendor registration."
    "wifi_dji_ie" -> "The scanner recognized a DJI information element in a Wi-Fi broadcast."
    "wifi_rid", "wifi_beacon_rid" -> "The scanner recognized Remote ID data in a Wi-Fi broadcast."
    "wifi_assoc" -> "The scanner flagged Wi-Fi traffic and supplied no further rule details."
    "wifi_inventory", "wifi_ap_inventory" -> "The scanner flagged an observed Wi-Fi access point and supplied no further rule details."
    else -> null
}

internal fun String.readableWifiReportReason(ssid: String?): String? {
    val text = trim()
    if (text == ssid || text.startsWith("ssid ", ignoreCase = true) || text.none(Char::isWhitespace)) return null
    return when (text.lowercase()) {
        "wifi deauth frames" -> "The scanner observed Wi-Fi deauthentication frames."
        "wifi disassoc frames" -> "The scanner observed Wi-Fi disassociation frames."
        "wifi beacon spam" -> "The scanner flagged a burst of Wi-Fi beacon traffic."
        "flock registered oui" -> "The Wi-Fi address prefix is registered to Flock Safety."
        "wifi frame evidence", "wifi ssid pattern", "wifi oui", "dji vendor ie", "wifi remote id" -> null
        else -> text
    }
}
