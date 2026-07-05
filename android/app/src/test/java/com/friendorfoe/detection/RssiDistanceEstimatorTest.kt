package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RssiDistanceEstimatorTest {

    @Test
    fun wifiDroneProfileUsesDeploymentScaleFallback() {
        assertEquals(15.0, RssiDistanceEstimator.estimateWifiDrone(-55), 1.0)
        assertEquals(135.0, RssiDistanceEstimator.estimateWifiDrone(-78), 12.0)
        assertEquals(1200.0, RssiDistanceEstimator.estimateWifiDrone(-101), 160.0)
    }

    @Test
    fun bleRemoteIdProfileUsesRemoteIdTransmitScale() {
        assertEquals(8.0, RssiDistanceEstimator.estimateBleRemoteId(-59), 1.0)
        assertEquals(60.0, RssiDistanceEstimator.estimateBleRemoteId(-79), 6.0)
        assertTrue(RssiDistanceEstimator.estimateBleRemoteId(-39) < 2.0)
    }

    @Test
    fun bleRemoteIdUsesAdvertisedTransmitPowerWhenPresent() {
        val defaultDistance = RssiDistanceEstimator.estimateBleRemoteId(-79)
        val highPowerDistance = RssiDistanceEstimator.estimateBleRemoteId(-79, txPowerDbm = 12)

        assertTrue(highPowerDistance > defaultDistance)
    }
}
