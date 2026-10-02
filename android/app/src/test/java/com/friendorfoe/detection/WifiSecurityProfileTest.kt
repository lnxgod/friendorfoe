package com.friendorfoe.detection

import org.junit.Assert.*
import org.junit.Test

class WifiSecurityProfileTest {
    @Test
    fun modernEncryptedAndUnknownNetworksAreNotOpen() {
        for (caps in listOf(
            "[RSN-SAE-CCMP][ESS]", "[RSN-OWE-CCMP][ESS]", "[OWE_TRANSITION][ESS]",
            "[RSN-EAP/SHA256-CCMP][ESS]", "[RSN-EAP_SUITE_B_192-GCMP-256][ESS]",
            "[WAPI-CERT-SMS4][ESS]", "[RSN-DPP-CCMP][ESS]", "[RSN-?-CCMP][ESS]",
            "[FUTURE-AKM][ESS]", "", "[ESS", "garbage", "[PRIVACY][ESS]",
        )) {
            assertFalse(caps, WifiSecurityProfile.parse(caps).open)
        }
        assertFalse(WifiSecurityProfile.parse(null).open)
        assertTrue(WifiSecurityProfile.parse("[ESS][WPS]").open)
        assertTrue(WifiSecurityProfile.parse("[ESS][UTF-8]").open)
    }

    @Test
    fun reportsLegacySupportEvenWhenCcmpIsAlsoOffered() {
        assertEquals(listOf("WEP"), WifiSecurityProfile.parse("[WEP][ESS]").obsoleteModes)
        assertEquals(listOf("TKIP"), WifiSecurityProfile.parse("[WPA2-PSK-CCMP+TKIP][ESS]").obsoleteModes)
        assertTrue(WifiSecurityProfile.parse("[WPA2-PSK-CCMP][ESS]").obsoleteModes.isEmpty())
        assertEquals("WPA2/WPA3", WifiSecurityProfile.parse("[RSN-PSK+SAE-CCMP][ESS]").label)
    }

    @Test
    fun flagsWeakSecurityOncePerApWithActionableEvidence() {
        val networks = listOf(network("[WEP][ESS]"), network("[WEP][ESS]"))
        val finding = WifiAnomalyDetector.analyzeNetworksForTest(networks).single()
        assertEquals("weak_security", finding.type)
        assertEquals(2, finding.threatLevel)
        assertTrue(finding.details.contains("WEP"))
        assertTrue(finding.details.contains("CCMP"))
        assertEquals(-55, finding.evidence.single().rssi)
    }

    @Test
    fun onlyDistinctOpenAndAuthenticatedApsTriggerMixedSecurity() {
        for (caps in listOf("[RSN-SAE-CCMP][ESS]", "[OWE_TRANSITION][ESS]", "[RSN-OWE-CCMP][ESS]", "[RSN-?-CCMP][ESS]", null)) {
            assertTrue(WifiAnomalyDetector.analyzeNetworksForTest(listOf(
                network("[WPA2-PSK-CCMP][ESS]"), network(caps, "00:00:00:00:00:02"),
            )).isEmpty())
        }
        assertTrue(WifiAnomalyDetector.analyzeNetworksForTest(listOf(network("[ESS]"), network("[WPA2-PSK-CCMP][ESS]"))).isEmpty())
        val finding = WifiAnomalyDetector.analyzeNetworksForTest(listOf(
            network("[ESS]"), network("[RSN-SAE-CCMP][ESS]", "00:00:00:00:00:02"),
        )).single()
        assertEquals("evil_twin", finding.type)
        assertEquals(2, finding.threatLevel)
        assertTrue(finding.details.contains("legitimate"))
    }

    private fun network(caps: String?, bssid: String = "00:00:00:00:00:01") =
        WifiAnomalyDetector.WifiNetwork("Cafe", bssid, caps, -55, 2437)
}
