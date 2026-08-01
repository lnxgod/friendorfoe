package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Test

class WifiScanReadinessTest {

    @Test
    fun android_13_requires_fine_location_and_nearby_wifi() {
        val base = WifiScanAccessSnapshot(
            sdkInt = 33,
            fineLocationGranted = true,
            nearbyWifiGranted = true,
            locationServicesEnabled = true,
            wifiEnabled = true,
        )

        assertEquals(WifiScanReadiness.READY, base.evaluate())
        assertEquals(
            WifiScanReadiness.MISSING_FINE_LOCATION,
            base.copy(fineLocationGranted = false).evaluate(),
        )
        assertEquals(
            WifiScanReadiness.MISSING_NEARBY_WIFI_DEVICES,
            base.copy(nearbyWifiGranted = false).evaluate(),
        )
    }

    @Test
    fun pre_android_13_does_not_require_nearby_wifi() {
        val snapshot = WifiScanAccessSnapshot(
            sdkInt = 32,
            fineLocationGranted = true,
            nearbyWifiGranted = false,
            locationServicesEnabled = true,
            wifiEnabled = true,
        )

        assertEquals(WifiScanReadiness.READY, snapshot.evaluate())
    }

    @Test
    fun disabled_location_and_wifi_are_reported_before_scan() {
        val base = WifiScanAccessSnapshot(
            sdkInt = 34,
            fineLocationGranted = true,
            nearbyWifiGranted = true,
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
