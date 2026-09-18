package com.friendorfoe.presentation.trails

import android.graphics.Bitmap
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Rule
import org.junit.Test
import java.io.File
import kotlin.math.sin

class FlightPathScreenTest {
    @get:Rule val compose = createComposeRule()
    private val now = System.currentTimeMillis()
    private fun points() = (0..12).map { i -> TrackingEntity(
        objectId = "demo", latitude = 32.71 + sin(i / 3.0) * 0.02,
        longitude = -117.18 + i * 0.005, altitudeMeters = 1200.0 + i * 10,
        heading = 90f, speedMps = 90f, timestamp = now - (12 - i) * 30_000L,
    ) }

    @Test fun pathDisplaysRecordedPositionsAndSupportsTimelineScrubbing() {
        compose.setContent {
            FriendOrFoeTheme { FlightPathContent(FlightPathState("DEMO123", points(), now, false), {}) }
        }
        compose.onNodeWithText("DEMO123").assertIsDisplayed()
        compose.onNodeWithTag("flight_path_map").assertIsDisplayed()
        compose.onNodeWithTag("flight_path_scrubber").performScrollTo().performTouchInput { swipeRight() }
        compose.onNodeWithText("First:", substring = true).assertIsDisplayed()
        compose.onNodeWithText("DEMO123").performScrollTo()
        // Native map layout catches up after the Compose scroll before visual capture.
        compose.waitForIdle()
        androidx.test.platform.app.InstrumentationRegistry.getInstrumentation().waitForIdleSync()
        saveScreenshot("flight-path.png")
    }

    @Test fun shorterWindowTruthfullyShowsNoRecordedPositions() {
        val old = points().map { it.copy(timestamp = it.timestamp - 3_600_000L) }
        compose.setContent {
            FriendOrFoeTheme { FlightPathContent(FlightPathState("DEMO123", old, now, false), {}) }
        }
        compose.onNodeWithText("15 min").performClick()
        compose.onNodeWithText("No recorded positions in this period").assertIsDisplayed()
        compose.onNodeWithTag("flight_path_map").assertDoesNotExist()
    }

    @Test fun reviewStaysOnSelectedPointUntilLatestIsRequested() {
        val state = mutableStateOf(FlightPathState("DEMO123", points(), now, false))
        compose.setContent { FriendOrFoeTheme { FlightPathContent(state.value, {}) } }
        compose.onNodeWithTag("flight_path_previous").performScrollTo().performClick()
        compose.onNodeWithText("Reviewing position 12 of 13").performScrollTo().assertIsDisplayed()
        compose.runOnIdle {
            val next = state.value.points.last().copy(timestamp = now + 30_000, longitude = -117.1)
            state.value = state.value.copy(points = state.value.points + next, nowMs = now + 30_000)
        }
        compose.onNodeWithText("Reviewing position 12 of 14").assertIsDisplayed()
        compose.onNodeWithTag("flight_path_next").performScrollTo().performClick()
        compose.onNodeWithText("Reviewing position 13 of 14").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("flight_path_latest").performScrollTo().performClick()
        compose.onNodeWithText("Following latest received position").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("flight_path_latest").assertIsNotEnabled()
        compose.onNodeWithTag("flight_path_next").assertIsNotEnabled()
        compose.runOnIdle {
            val next = state.value.points.last().copy(timestamp = now + 60_000)
            state.value = state.value.copy(points = state.value.points + next, nowMs = now + 60_000)
        }
        compose.onNodeWithTag("flight_path_next").assertIsNotEnabled()
        compose.onNodeWithTag("flight_path_latest").assertIsNotEnabled()
        compose.mainClock.advanceTimeBy(600)
        compose.waitForIdle()
        saveScreenshot("flight-path-controls.png")
    }

    private fun saveScreenshot(name: String) {
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, name).outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
    }
}
