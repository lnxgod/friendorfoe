package com.friendorfoe

import android.Manifest
import android.content.pm.PackageManager
import androidx.compose.material3.Text
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.core.content.ContextCompat
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class FreshLaunchPermissionTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun freshProcessStaysAliveWithoutLocalDetectionPermissions() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        listOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.NEARBY_WIFI_DEVICES,
            Manifest.permission.RECORD_AUDIO,
        ).forEach { permission ->
            assertEquals(
                "$permission must begin denied",
                PackageManager.PERMISSION_DENIED,
                ContextCompat.checkSelfPermission(context, permission),
            )
        }

        compose.setContent { Text("Fresh launch is stable") }
        compose.onNodeWithText("Fresh launch is stable").assertIsDisplayed()
    }
}
