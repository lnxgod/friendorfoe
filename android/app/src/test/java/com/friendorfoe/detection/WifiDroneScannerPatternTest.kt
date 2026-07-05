package com.friendorfoe.detection

import com.friendorfoe.presentation.util.DroneDatabase
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiDroneScannerPatternTest {

    @Test
    fun android_drone_patterns_drop_broad_holy_and_ufo_prefixes() {
        assertNull(WifiDroneScanner.matchDroneManufacturerForTest("HolyCowGuest"))
        assertNull(WifiDroneScanner.matchDroneManufacturerForTest("UFO-Arcade"))
        assertTrue(DroneDatabase.matchByWifiSsid("HolyCowGuest").isEmpty())
    }

    @Test
    fun android_drone_patterns_keep_specific_holy_stone_and_wifi_ufo_variants() {
        assertEquals(
            "Holy Stone",
            WifiDroneScanner.matchDroneManufacturerForTest("HolyStoneEIS-1234")
        )
        assertEquals(
            "Generic",
            WifiDroneScanner.matchDroneManufacturerForTest("WiFiUFO-1234")
        )
        assertTrue(DroneDatabase.matchByWifiSsid("HolyStoneFPV_1234").isNotEmpty())
    }

    @Test
    fun android_drone_patterns_keep_remote_id_and_dji_quicktransfer_variants() {
        assertEquals(
            "DJI",
            WifiDroneScanner.matchDroneManufacturerForTest("RID-1234567890")
        )
        assertEquals(
            "DJI",
            WifiDroneScanner.matchDroneManufacturerForTest("DJI-Mini4Pro-123456")
        )
        assertEquals(
            "DJI",
            WifiDroneScanner.matchDroneManufacturerForTest("DJI_Neo_123456")
        )
    }
}
