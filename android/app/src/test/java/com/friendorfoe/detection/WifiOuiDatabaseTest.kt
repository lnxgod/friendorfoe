package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
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

    @Test
    fun assigns_explicit_roles_without_treating_privacy_devices_as_drones() {
        mapOf(
            "E0:A7:00:11:22:33" to "Verkada",
            "CC:47:BD:11:22:33" to "Rhombus",
            "00:25:DF:11:22:33" to "Axon",
            "2C:42:05:11:22:33" to "Lytx",
            "50:DF:95:11:22:33" to "Lytx",
            "58:A7:48:11:22:33" to "Lytx",
            "70:E4:6E:11:22:33" to "Lytx"
        ).forEach { (bssid, manufacturer) ->
            val entry = WifiOuiDatabase.lookup(bssid)
            assertNotNull(entry)
            assertEquals(manufacturer, entry!!.manufacturer)
            assertEquals(OuiRole.PRIVACY_INFRASTRUCTURE, entry.role)
            assertFalse(WifiOuiDatabase.isDroneOui(bssid))
        }

        val flock = WifiOuiDatabase.lookup("B4:1E:52:11:22:33")
        assertEquals(OuiRole.PRIVACY_FLOCK, flock?.role)
        assertFalse(WifiOuiDatabase.isDroneOui("B4:1E:52:11:22:33"))

        assertEquals(OuiRole.DRONE, WifiOuiDatabase.lookup("60:60:1F:11:22:33")?.role)
        assertTrue(WifiOuiDatabase.isDroneOui("60:60:1F:11:22:33"))

        assertEquals(OuiRole.ENRICHMENT_ONLY, WifiOuiDatabase.lookup("24:0A:C4:11:22:33")?.role)
        assertFalse(WifiOuiDatabase.isDroneOui("24:0A:C4:11:22:33"))
    }
}
