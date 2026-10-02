package com.friendorfoe.detection

import java.util.Locale

/** Interprets Android ScanResult.capabilities, without treating unknown AKMs as open. */
data class WifiSecurityProfile(
    val label: String,
    val authenticated: Boolean = false,
    val open: Boolean = false,
    val obsoleteModes: List<String> = emptyList(),
) {
    companion object {
        fun parse(capabilities: String?): WifiSecurityProfile {
            val caps = capabilities?.trim()?.uppercase(Locale.ROOT).orEmpty()
            if (caps.isEmpty()) return WifiSecurityProfile("UNKNOWN")
            val tokens = caps.split(Regex("[^A-Z0-9_]+")).filter(String::isNotBlank).toSet()
            val groups = Regex("\\[([^\\[\\]]+)\\]").findAll(caps).map { it.groupValues[1] }.toList()
            val obsolete = listOf("WEP", "TKIP").filter { it in tokens }
            val label = when {
                "OWE_TRANSITION" in tokens -> "OWE transition"
                "OWE" in tokens -> "OWE"
                "WEP" in tokens -> "WEP"
                "WAPI" in tokens -> "WAPI"
                "SAE" in tokens && "PSK" in tokens -> "WPA2/WPA3"
                "SAE" in tokens -> "WPA3"
                "EAP_SUITE_B_192" in tokens || "SUITE_B_192" in tokens -> "WPA3 Enterprise"
                "DPP" in tokens -> "DPP"
                "WPA3" in tokens -> "WPA3"
                "WPA2" in tokens -> "WPA2"
                "RSN" in tokens -> "RSN"
                "WPA" in tokens -> "WPA"
                "PSK" in tokens || "EAP" in tokens -> "Secured"
                // A missing, malformed, or unknown security field is not evidence of openness.
                caps == groups.joinToString("") { "[$it]" } &&
                    "ESS" in groups && groups.all { it in setOf("ESS", "WPS", "UTF-8", "WPS-PBC", "WPS-PIN") } -> "OPEN"
                else -> "UNKNOWN"
            }
            val authenticated = tokens.any {
                it in setOf("WEP", "PSK", "SAE", "EAP", "EAP_SUITE_B_192", "SUITE_B_192", "DPP")
            } && "OWE_TRANSITION" !in tokens && "OWE" !in tokens
            return WifiSecurityProfile(label, authenticated, label == "OPEN", obsolete)
        }
    }
}
