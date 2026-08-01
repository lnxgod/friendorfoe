package com.friendorfoe.detection

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiDroneScannerIdentityContractTest {

    @Test
    fun scanner_uses_validated_radio_identity_without_dropping_ssid_fallbacks() {
        val source = scannerSource()

        assertFalse(source.contains("val bssid = result.BSSID ?: continue"))
        assertTrue(source.contains("WifiTransmitterIdentity.normalize(result.BSSID)"))
        assertTrue(source.contains("WifiTransmitterIdentity.identityKey(ssid, bssid)"))
        assertTrue(source.contains("WifiTransmitterIdentity.detectionId(\"wifi_dji\", ssid, bssid)"))
        assertTrue(source.contains("WifiTransmitterIdentity.detectionId(\"wifi\", ssid, bssid)"))
        assertTrue(source.contains("WifiTransmitterIdentity.detectionId(\"wifi_soft\", ssid, bssid)"))
        assertTrue(source.contains("bssid?.let { WifiOuiDatabase.lookup(it) }"))
    }

    private fun scannerSource(): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt"),
            File("app/src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt"),
            File("android/app/src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt"),
        )
        return candidates.first { it.isFile }.readText()
    }
}
