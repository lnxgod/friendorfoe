package com.friendorfoe.presentation.permissions

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import com.friendorfoe.presentation.ar.ArSystemStatusFrame
import com.friendorfoe.presentation.navigation.FofNavigationSuite
import com.friendorfoe.presentation.navigation.ArPermissionRoute
import com.friendorfoe.presentation.navigation.TopLevelDestination
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.welcome.WelcomeScreen
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Rule
import org.junit.Test

class ContextualPermissionTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun welcomeShowsNoPermissionGate() {
        compose.setContent {
            FriendOrFoeTheme { WelcomeScreen(onGetStarted = {}) }
        }

        compose.onNodeWithText("Continue").assertIsDisplayed()
        listOf("Camera for AR", "Nearby-device access", "Notifications", "Microphone")
            .forEach { text ->
                compose.onNodeWithText(text, substring = true).assertDoesNotExist()
            }
    }

    @Test
    fun deniedCameraDoesNotBlockNavigationToInfo() {
        var selected by mutableStateOf(TopLevelDestination.AR)
        compose.setContent {
            FriendOrFoeTheme {
                FofNavigationSuite(
                    showNavigation = true,
                    currentRoute = selected.route,
                    onNavigate = { selected = it },
                ) { padding ->
                    Box(Modifier.padding(padding).fillMaxSize()) {
                        if (selected == TopLevelDestination.AR) {
                            FeaturePermissionGate(
                                feature = AppFeature.AR_CAMERA,
                                state = PermissionUiState.PermanentlyDenied,
                                onRequest = {},
                                onOpenSettings = {},
                                grantedContent = {},
                            )
                        } else if (selected == TopLevelDestination.INFO) {
                            Box(Modifier.fillMaxSize().testTag("screen_info")) { Text("Info") }
                        }
                    }
                }
            }
        }

        compose.onNodeWithText("Camera for AR").assertIsDisplayed()
        compose.onNodeWithContentDescription("Info").performClick()
        compose.onNodeWithTag("screen_info").assertIsDisplayed()
    }

    @Test
    fun deniedToggleStaysOffAndOpensExplanation() {
        var enabled by mutableStateOf(false)
        var explanationRequests = 0
        compose.setContent {
            FriendOrFoeTheme {
                PermissionBackedToggle(
                    tag = "military_alerts",
                    label = "Military alerts",
                    description = "Notify for nearby military aircraft",
                    checked = enabled,
                    permissionState = PermissionUiState.Denied,
                    onOpenExplanation = { explanationRequests += 1 },
                    onCommitChecked = { enabled = it },
                )
            }
        }

        compose.onNodeWithTag("military_alerts").performClick()

        compose.onNodeWithTag("military_alerts").assertIsOff()
        assertEquals(1, explanationRequests)
        assertFalse(enabled)
    }

    @Test
    fun routeExplanationPrecedesPermissionRequest() {
        var requests = 0
        compose.setContent {
            FriendOrFoeTheme {
                FeaturePermissionGate(
                    feature = AppFeature.PHONE_PRIVACY_SCAN,
                    state = PermissionUiState.Denied,
                    onRequest = { requests += 1 },
                    onOpenSettings = {},
                    grantedContent = {},
                )
            }
        }

        assertEquals(0, requests)
        compose.onNodeWithText("Continue").performClick()
        assertEquals(1, requests)
    }

    @Test
    fun deniedAndApproximateLocationStillReachArCameraContent() {
        var locationState by mutableStateOf<PermissionUiState>(PermissionUiState.Denied)
        compose.setContent {
            FriendOrFoeTheme {
                ArPermissionRoute(
                    cameraState = PermissionUiState.Granted,
                    locationState = locationState,
                    onRequestCamera = {},
                    onOpenCameraSettings = {},
                ) { resolvedLocation ->
                    Text("AR camera content · ${resolvedLocation::class.simpleName}")
                }
            }
        }

        compose.onNodeWithText("AR camera content · Denied").assertIsDisplayed()
        compose.runOnIdle { locationState = PermissionUiState.Approximate }
        compose.onNodeWithText("AR camera content · Approximate").assertIsDisplayed()
    }

    @Test
    fun gpsAndOptionalLocationStatusNeverReplaceArCameraContent() {
        var locationRequests = 0
        var locationState by mutableStateOf<PermissionUiState>(PermissionUiState.Denied)
        compose.setContent {
            FriendOrFoeTheme {
                ArSystemStatusFrame(
                    gpsEnabled = locationState == PermissionUiState.Denied,
                    locationPermissionState = locationState,
                    isOnline = true,
                    sensorAccuracy = android.hardware.SensorManager.SENSOR_STATUS_ACCURACY_HIGH,
                    onRequestLocation = { locationRequests += 1 },
                    onOpenLocationSettings = {},
                    onOpenSystemLocationSettings = {},
                ) {
                    Text("AR camera content")
                }
            }
        }

        compose.onNodeWithText("AR camera content").assertIsDisplayed()
        compose.onNodeWithText("Location is optional", substring = true).performClick()
        compose.runOnIdle {
            assertEquals(1, locationRequests)
            locationState = PermissionUiState.Granted
        }
        compose.onNodeWithText("Location services are off", substring = true).assertIsDisplayed()
        compose.onNodeWithText("AR camera content").assertIsDisplayed()
        compose.onNodeWithText("Bluetooth", substring = true).assertDoesNotExist()
    }
}
