package com.friendorfoe.presentation

import android.graphics.Bitmap
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import com.friendorfoe.presentation.list.*
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.trails.*
import com.friendorfoe.domain.model.*
import com.friendorfoe.data.local.TrackingEntity
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import java.time.Instant

class NearbyUpgradesTest {
    private fun capture(name: String) {
        compose.mainClock.advanceTimeBy(600)
        compose.waitForIdle()
        InstrumentationRegistry.getInstrumentation().waitForIdleSync()
        android.os.SystemClock.sleep(1000) // Include the Android window frame and asynchronous map tiles.
        val bitmap = InstrumentationRegistry.getInstrumentation().uiAutomation.takeScreenshot()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, "nearby-upgrade-$name").outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        bitmap.recycle()
    }

    @get:Rule val compose = createComposeRule()
    private fun aircraft(id: String, miles: Double, category: ObjectCategory) = Aircraft(id = id, icaoHex = id,
        callsign = id, position = Position(32.7, -117.1, 1000.0), category = category,
        firstSeen = Instant.now(), lastUpdated = Instant.now(), distanceMeters = miles * 1609.344)

    @Test fun sortChoiceReordersRowsAndRangePresetChangesGrouping() {
        var nearest by mutableStateOf(false)
        var miles by mutableIntStateOf(10)
        val rows = listOf(aircraft("HELI", 9.0, ObjectCategory.HELICOPTER), aircraft("CLOSE", 1.0, ObjectCategory.COMMERCIAL))
        compose.setContent { FriendOrFoeTheme { Surface { ListContent(ListUiState(
            body = ListBodyState.Results(sortSkyObjectsForList(rows, emptySet(), miles, nearest)),
            locationPermissionState = PermissionUiState.Granted, nearestFirst = nearest, aircraftRangeMiles = miles),
            ListActions(onSetNearestFirst = { nearest = it }, onSetAircraftRangeMiles = { miles = it })) } } }
        capture("nearby-priority.png")
        compose.onNodeWithTag("sort_nearest").performClick()
        compose.runOnIdle { assertTrue(nearest) }
        assertTrue(compose.onNodeWithTag("list_row_CLOSE").fetchSemanticsNode().boundsInRoot.top <
            compose.onNodeWithTag("list_row_HELI").fetchSemanticsNode().boundsInRoot.top)
        compose.onNodeWithTag("sort_priority").performClick()
        compose.onNodeWithTag("nearby_range").performClick()
        compose.onNodeWithTag("range_preset_5").performClick()
        compose.onNodeWithText("Done").performClick()
        compose.onNodeWithText("Within 5 mi").assertIsDisplayed()
        compose.onNodeWithText("Farther away").assertIsDisplayed()
    }

    @Test fun customRangeRequiresValidMilesAndAppliesExactValue() {
        var miles by mutableIntStateOf(10)
        compose.setContent { FriendOrFoeTheme { Surface { ListContent(ListUiState(aircraftRangeMiles = miles), ListActions(onSetAircraftRangeMiles = { miles = it })) } } }
        compose.onNodeWithTag("nearby_range").performClick()
        capture("range.png")
        compose.onNodeWithText("Custom").performClick()
        compose.onNodeWithTag("custom_range_input").performTextReplacement("51")
        compose.onNodeWithText("Apply").assertIsNotEnabled()
        compose.onNodeWithTag("custom_range_input").performTextReplacement("12")
        compose.onNodeWithText("Apply").performClick()
        compose.runOnIdle { assertEquals(12, miles) }
    }

    @Test fun offlineStatusOffersWorkingRetry() {
        var retried = false
        compose.setContent { FriendOrFoeTheme { Surface { ListContent(
            ListUiState(body = ListBodyState.Failed("Aircraft offline"), feedLabel = "Aircraft offline", canRetryFeed = true),
            ListActions(onRetryFeed = { retried = true })) } } }
        compose.onAllNodesWithText("Retry").onFirst().performClick()
        compose.runOnIdle { assertTrue(retried) }
        compose.onNodeWithText("No nearby detections").assertDoesNotExist()
    }

    @Test fun savedFlightOlderThanOneDayRemainsReviewableAndExportUsesItsPositions() {
        val points = (0..2).map { TrackingEntity(objectId = "saved", latitude = 32.7 + it * 0.001, longitude = -117.1 + it * 0.003,
            altitudeMeters = 1000.0 + it * 10, heading = null, speedMps = 50f,
            timestamp = System.currentTimeMillis() - 48 * 3_600_000L + it * 10_000) }
        var exported: List<TrackingEntity>? = null
        compose.setContent { FriendOrFoeTheme { FlightPathContent(
            FlightPathState("Saved flight", points, System.currentTimeMillis(), false, isSaved = true), {},
            onExport = { exported = it }) } }
        compose.onNodeWithTag("flight_path_map").assertIsDisplayed()
        capture("saved-flight-map.png")
        compose.onNodeWithTag("export_flight").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(points, exported) }
        capture("flight-controls.png")
        compose.onNodeWithTag("save_flight").assertDoesNotExist()
        compose.onNodeWithTag("flight_play_pause").performScrollTo().performClick()
        compose.onNodeWithText("Pause").assertIsDisplayed().performClick()
        compose.onNodeWithText("Play flight").assertIsDisplayed()
        compose.onNodeWithText("Speed ·", substring = true).performScrollTo()
        capture("flight-charts.png")
    }
}
