package com.friendorfoe.presentation

import android.graphics.Bitmap
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.data.remote.*
import com.friendorfoe.presentation.probes.*
import com.friendorfoe.presentation.navigation.AboutTopLevelRoute
import com.friendorfoe.presentation.navigation.Screen
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import java.io.File

class ProbeScreenTest {
    @get:Rule val compose = createComposeRule()

    @Test fun setupExplainsHardwareAndOpensSettings() {
        var opened = false
        compose.setContent { FriendOrFoeTheme { Surface { ProbeContent(ProbeUiState(), ProbeActions(onSettings = { opened = true })) } } }
        compose.onNodeWithText("Connect a probe-capable scanner").assertIsDisplayed()
        compose.onNodeWithText("Open app settings").performClick()
        compose.runOnIdle { assertTrue(opened) }
        capture("setup")
    }

    @Test fun scannerSelectionSearchAndWildcardDisplay() {
        var state by mutableStateOf(ProbeUiState(enabled = true, snapshotElapsedMs = 100, nowElapsedMs = 100,
            snapshot = ProbeActivityDto(observers = listOf(ProbeObserverDto("desk-scanner", 1.0)))))
        compose.setContent { FriendOrFoeTheme { Surface { ProbeContent(state, ProbeActions(
            onSelectSensor = { id -> state = state.copy(sensorId = id, snapshot = state.snapshot.copy(sensorId = id, transmitters = listOf(
                ProbeTransmitterDto("02:11:22:33:44:55", id, true, reports = 4, ageSeconds = 2.0, rssi = -48, channel = 6,
                    targets = listOf(ProbeTargetDto("Home, Wi-Fi"), ProbeTargetDto("Office Guest"))),
                ProbeTransmitterDto("04:11:22:33:44:66", id, wildcardReports = 2, reports = 2, ageSeconds = 20.0, rssi = -70)))) },
            onQuery = { state = state.copy(query = it) },
        )) } } }
        compose.onNodeWithTag("probe_scanner").performClick()
        compose.onNodeWithText("desk-scanner").performClick()
        compose.onNodeWithText("Home, Wi-Fi").performScrollTo().assertIsDisplayed()
        capture("activity")
        compose.onNodeWithText("Any network · wildcard scan").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("probe_search").performScrollTo().performTextInput("Office")
        compose.onNodeWithText("Home, Wi-Fi").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Any network · wildcard scan").assertDoesNotExist()
    }

    @Test fun moreNavigationOpensProbesAndBackReturnsToMore() {
        compose.setContent { FriendOrFoeTheme {
            val nav = rememberNavController()
            NavHost(nav, startDestination = Screen.About.route) {
                composable(Screen.About.route) { AboutTopLevelRoute(nav) }
                composable(Screen.Probes.route) {
                    ProbeContent(ProbeUiState(), ProbeActions(onBack = { nav.popBackStack() }))
                }
            }
        } }
        compose.onNodeWithTag("more_probes").performClick()
        compose.onNodeWithTag("probe_screen").assertIsDisplayed()
        compose.onNodeWithContentDescription("Back").performClick()
        compose.onNodeWithTag("about_landing").assertIsDisplayed()
    }

    @Test fun usbSetupAndLiveReportsDoNotRequireBackendSettings() {
        var connected = false
        var state by mutableStateOf(ProbeUiState(enabled = true, usbSource = true))
        compose.setContent { FriendOrFoeTheme { Surface { ProbeContent(state, ProbeActions(
            onRetry = { connected = true }, onSelectUsb = { state = state.copy(usbSource = it) }
        )) } } }
        compose.onNodeWithText("USB Wi-Fi probe scanner").assertIsDisplayed()
        compose.onNodeWithText("Get scanner firmware").assertIsDisplayed()
        compose.onNodeWithText("Connect USB").performClick()
        compose.runOnIdle { assertTrue(connected) }
        capture("usb-setup")
        compose.runOnIdle {
            state = state.copy(sensorId = "usb-local", snapshotElapsedMs = 0,
                usb = com.friendorfoe.data.probes.UsbProbeState(connected = true, message = "USB live · 2.4 GHz · channels 1–13"),
                snapshot = ProbeActivityDto("usb-local", transmitters = listOf(
                    ProbeTransmitterDto("02:11:22:33:44:55", "usb-local", reports = 3, rssi = -48, channel = 6,
                        targets = listOf(ProbeTargetDto("Home, Wi-Fi"))),
                    ProbeTransmitterDto("04:11:22:33:44:66", "usb-local", reports = 2, wildcardReports = 2, rssi = -70, channel = 1))))
        }
        compose.onNodeWithText("Scanner connected").assertIsDisplayed()
        compose.onNodeWithText("Home, Wi-Fi").performScrollTo().assertIsDisplayed()
        capture("usb-live")
        compose.onNodeWithTag("probe_scanner").assertDoesNotExist()
        compose.onNodeWithText("Any network · wildcard scan").performScrollTo().assertIsDisplayed()
    }

    private fun capture(name: String) {
        compose.waitForIdle()
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val image = instrumentation.uiAutomation.takeScreenshot()
        File(instrumentation.targetContext.filesDir, "wifi-probe-$name.png").outputStream().use {
            image.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        image.recycle()
    }
}
