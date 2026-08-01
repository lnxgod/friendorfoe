package com.friendorfoe.data.repository

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SkyObjectRepositoryPrivacyOwnershipTest {

    @Test
    fun skyRepositoryPublishesPermissionsButDoesNotOwnPrivacyCollectors() {
        val dependencyNames = SkyObjectRepository::class.java.declaredConstructors
            .flatMap { constructor -> constructor.parameterTypes.toList() }
            .map { type -> type.name }
            .toSet()

        assertTrue(LocalDetectionPermissionUpdates::class.java.name in dependencyNames)
        assertFalse("com.friendorfoe.detection.GlassesDetector" in dependencyNames)
        assertFalse("com.friendorfoe.detection.WifiPrivacyScanner" in dependencyNames)
        assertFalse("com.friendorfoe.detection.UltrasonicDetector" in dependencyNames)
        assertFalse("com.friendorfoe.sensor.SensorFusionEngine" in dependencyNames)
    }
}
