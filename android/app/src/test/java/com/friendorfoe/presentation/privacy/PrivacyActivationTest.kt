package com.friendorfoe.presentation.privacy

import android.app.Activity
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.presentation.about.InfoSettingKey
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyActivationTest {
    @Test
    fun leavingBackendOnlyUsesTheSettingsTransactionThatRestartsCollectors() {
        assertEquals(
            listOf(
                InfoSettingKey.PHONE_PRIVACY_SCAN to true,
                InfoSettingKey.BACKEND_ONLY to false,
            ),
            phonePrivacyEnableWrites(
                DetectionSettings.defaults().copy(backendOnlyMode = true),
            ),
        )
        assertEquals(
            listOf(InfoSettingKey.PHONE_PRIVACY_SCAN to true),
            phonePrivacyEnableWrites(
                DetectionSettings.defaults().copy(backendOnlyMode = false),
            ),
        )
    }

    @Test
    fun BluetoothScannerRetriesOnlyAfterUserAllowsEnableRequest() {
        assertTrue(shouldRetryAfterBluetoothEnable(Activity.RESULT_OK))
        assertFalse(shouldRetryAfterBluetoothEnable(Activity.RESULT_CANCELED))
    }
}
