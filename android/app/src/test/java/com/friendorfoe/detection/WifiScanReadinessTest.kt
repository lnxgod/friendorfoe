package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Test

class WifiScanReadinessTest {

    @Test
    fun wifiManagerScanRequiresFineLocation() {
        val base = WifiScanAccessSnapshot(true, true, true)
        assertEquals(WifiScanReadiness.READY, base.evaluate())
        assertEquals(WifiScanReadiness.MISSING_FINE_LOCATION, base.copy(fineLocationGranted = false).evaluate())
    }

    @Test
    fun disabled_location_and_wifi_are_reported_before_scan() {
        val base = WifiScanAccessSnapshot(
            fineLocationGranted = true,
            locationServicesEnabled = true,
            wifiEnabled = true,
        )

        assertEquals(
            WifiScanReadiness.LOCATION_SERVICES_DISABLED,
            base.copy(locationServicesEnabled = false).evaluate(),
        )
        assertEquals(
            WifiScanReadiness.WIFI_DISABLED,
            base.copy(wifiEnabled = false).evaluate(),
        )
    }
}
