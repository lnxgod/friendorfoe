package com.friendorfoe.sensor

import android.hardware.Sensor
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SensorFusionReferenceTest {
    @Test
    fun gameRotationYawIsNeverClaimedAsACompassReference() {
        assertFalse(
            orientationHasCompassReference(
                vectorSensorType = Sensor.TYPE_GAME_ROTATION_VECTOR,
                hasAccelMagSolution = false,
            ),
        )
    }

    @Test
    fun magneticRotationVectorAndAccelMagSolutionsAreCompassReferenced() {
        assertTrue(
            orientationHasCompassReference(
                vectorSensorType = Sensor.TYPE_ROTATION_VECTOR,
                hasAccelMagSolution = false,
            ),
        )
        assertTrue(
            orientationHasCompassReference(
                vectorSensorType = null,
                hasAccelMagSolution = true,
            ),
        )
    }
}
