package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class WifiOuiDatabaseTest {

    @Test
    fun normalizes_common_mac_formats_for_oui_lookup() {
        val bssids = listOf(
            " 60:60:1f:aa:bb:cc ",
            "60-60-1F-AA-BB-CC",
            "60601FAABBCC",
            "6060.1FAA.BBCC"
        )

        bssids.forEach { bssid ->
            val entry = WifiOuiDatabase.lookup(bssid)
            assertNotNull("Expected DJI OUI for $bssid", entry)
            assertEquals("DJI", entry!!.manufacturer)
        }

        listOf(
            "X60:60:1F:AA:BB:CC",
            "60:60:1Z:AA:BB:CC"
        ).forEach { bssid ->
            assertNull(
                "Malformed near-match should not resolve OUI: $bssid",
                WifiOuiDatabase.lookup(bssid)
            )
        }
    }
}
