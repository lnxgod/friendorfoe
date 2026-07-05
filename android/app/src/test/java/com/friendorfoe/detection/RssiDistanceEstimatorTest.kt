package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RssiDistanceEstimatorTest {

    @Test
    fun wifiDroneProfileUsesDeploymentScaleFallback() {
        assertEquals(1.0, RssiDistanceEstimator.estimateWifiDrone(-55), 0.01)
        assertEquals(10.0, RssiDistanceEstimator.estimateWifiDrone(-78), 0.35)
        assertEquals(100.0, RssiDistanceEstimator.estimateWifiDrone(-101), 4.0)
    }

    @Test
    fun bleRemoteIdProfileUsesRemoteIdTransmitScale() {
        assertEquals(1.0, RssiDistanceEstimator.estimateBleRemoteId(-59), 0.01)
        assertEquals(10.0, RssiDistanceEstimator.estimateBleRemoteId(-79), 0.25)
        assertTrue(RssiDistanceEstimator.estimateBleRemoteId(-39) < 0.2)
    }
}
