package com.friendorfoe.data

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionSettingsTest {
    @Test
    fun snapshotDefaultsMatchCurrentProductDefaults() {
        val value = DetectionSettings.defaults()

        assertTrue(value.adsbEnabled)
        assertTrue(value.bleRidEnabled)
        assertTrue(value.wifiEnabled)
        assertTrue(value.stalkerEnabled)
        assertTrue(value.wifiAnomalyEnabled)
        assertTrue(value.sensorBackendEnabled)
        assertFalse(value.phonePrivacyScanEnabled)
        assertFalse(value.privacyNotificationsEnabled)
        assertFalse(value.droneAlertsEnabled)
        assertFalse(value.helicopterAlertsEnabled)
        assertFalse(value.militaryAlertsEnabled)
        assertFalse(value.policeAlertsEnabled)
        assertFalse(value.ultrasonicEnabled)
        assertFalse(value.backendOnlyMode)
    }
}
