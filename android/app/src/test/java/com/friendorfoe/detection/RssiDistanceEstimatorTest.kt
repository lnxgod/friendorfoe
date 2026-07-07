package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RssiDistanceEstimatorTest {

    @Test
    fun wifiDroneProfileUsesCalibratedOneMeterReference() {
        assertEquals(1.0, RssiDistanceEstimator.estimateWifiDrone(-55), 0.01)
        assertEquals(10.0, RssiDistanceEstimator.estimateWifiDrone(-78), 0.35)
        assertEquals(100.0, RssiDistanceEstimator.estimateWifiDrone(-101), 4.0)
    }

    @Test
    fun bleRemoteIdProfileUsesCalibratedBluetoothScannerScale() {
        assertEquals(1.0, RssiDistanceEstimator.estimateBleRemoteId(-59), 0.01)
        assertEquals(10.0, RssiDistanceEstimator.estimateBleRemoteId(-79), 0.25)
        assertTrue(RssiDistanceEstimator.estimateBleRemoteId(-39) < 0.2)
    }

    @Test
    fun advertisedTransmitPowerAdjustsPathLossWithoutReplacingCalibration() {
        val defaultDistance = RssiDistanceEstimator.estimateBleRemoteId(-79)
        val highPowerDistance = RssiDistanceEstimator.estimateBleRemoteId(-79, txPowerDbm = 12)
        val lowPowerDistance = RssiDistanceEstimator.estimateBleRemoteId(-79, txPowerDbm = -20)

        assertTrue(highPowerDistance > defaultDistance)
        assertEquals(40.0, highPowerDistance, 4.0)
        assertEquals(1.0, lowPowerDistance, 0.1)
    }

    @Test
    fun implausibleAdvertisedTransmitPowerFallsBackToProfileDefault() {
        val defaultDistance = RssiDistanceEstimator.estimateBleRemoteId(-79)

        assertEquals(defaultDistance, RssiDistanceEstimator.estimateBleRemoteId(-79, txPowerDbm = 126), 0.01)
    }
}
