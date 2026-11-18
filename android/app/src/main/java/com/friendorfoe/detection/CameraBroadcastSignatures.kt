package com.friendorfoe.detection

import java.util.UUID

/** Camera discovery broadcasts only. No connection, camera controls or recording-state inference. */
internal object CameraBroadcastSignatures {
    data class Match(
        val brand: String,
        val deviceType: String,
        val category: PrivacyCategory,
        val confidence: Float,
        val reason: String,
        val evidence: String,
    )

    const val LIMITATION = "This broadcast can be imitated. It does not show whether the camera is recording."

    // Ricoh lists this in the Advertising section, separately from its connected GATT services.
    // https://docs-theta-api.ricoh360.com/bluetooth-api/
    private val thetaAdvertisingUuid = UUID.fromString("f74d1ba5-bc52-4f10-99c0-85e785ebe43f")
    private val thetaBle = Match("Ricoh", "THETA 360° Camera", PrivacyCategory.ACTION_CAMERA, 0.90f,
        "uuid128:$thetaAdvertisingUuid", "Advertises the Ricoh THETA camera discovery service")

    fun advertisedService(uuid: UUID): Match? = if (uuid == thetaAdvertisingUuid) thetaBle else null

    // https://docs-theta-api.ricoh360.com/web-api/main.html — A1, X and Z1.
    private val thetaSsid = Regex("""THETA(?:AA|YR|YN)[0-9]{8}\.OSC""")
    // TP-Link setup guides: https://www.tp-link.com/us/support/faq/2663/ (Kasa)
    // and https://www.tp-link.com/us/support/faq/2747/ (Tapo).
    private val setupSsid = Regex("""(Tapo|Kasa)_Cam_[0-9a-fA-F]{4}""", RegexOption.IGNORE_CASE)

    fun wifiSsid(ssid: String): Match? {
        if (thetaSsid.matches(ssid)) return Match("Ricoh", "THETA 360° Camera",
            PrivacyCategory.ACTION_CAMERA, 0.80f, "wifi_ssid:theta_direct",
            "Network name matches a Ricoh THETA camera in direct Wi-Fi mode")
        val setup = setupSsid.matchEntire(ssid) ?: return null
        val family = if (setup.groupValues[1].equals("Tapo", ignoreCase = true)) "Tapo" else "Kasa"
        return Match("TP-Link", "$family Camera (setup network)", PrivacyCategory.SURVEILLANCE_CAMERA,
            0.80f, "wifi_ssid:${family}_Cam_", "Network name matches a $family camera in Wi-Fi setup mode")
    }
}
