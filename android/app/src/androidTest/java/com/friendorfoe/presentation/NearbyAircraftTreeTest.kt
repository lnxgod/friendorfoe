package com.friendorfoe.presentation

import android.graphics.Bitmap
import androidx.compose.material3.Surface
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.domain.model.*
import com.friendorfoe.presentation.list.*
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import java.io.File
import java.time.Instant

class NearbyAircraftTreeTest {
    @get:Rule val compose = createComposeRule()

    @Test
    fun nearbyGroupsFollowDistanceAndFiftyMileHelicopterStartsCollapsed() {
        val rows = listOf(plane("FAR50", 50.0, ObjectCategory.HELICOPTER),
            plane("HELI9", 9.0, ObjectCategory.HELICOPTER), plane("CLOSE1", 1.0))
        var opened: SkyObject? = null
        compose.setContent { FriendOrFoeTheme { Surface {
            ListContent(state(rows), ListActions(onOpenPeek = { opened = it }), activeVisualFocusIds = setOf("FAR50", "HELI9"))
        } } }
        compose.onNodeWithTag("list_row_CLOSE1").assertIsDisplayed()
        compose.onNodeWithTag("list_row_FAR50").assertDoesNotExist()
        val commercialTop = compose.onNodeWithTag("nearby_type_nearby_COMMERCIAL").fetchSemanticsNode().boundsInRoot.top
        val helicopterTop = compose.onNodeWithTag("nearby_type_nearby_HELICOPTERS").fetchSemanticsNode().boundsInRoot.top
        assertTrue(commercialTop < helicopterTop)
        capture("nearby-groups.png")
        clickBranch("nearby_section_farther")
        compose.onNodeWithTag("list_row_FAR50").assertDoesNotExist()
        clickBranch("nearby_type_farther_HELICOPTERS")
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_FAR50"))
        compose.onNodeWithTag("list_distance_FAR50", useUnmergedTree = true).assertTextEquals("50.0 mi")
        compose.onNodeWithTag("list_row_FAR50").performClick()
        compose.runOnIdle { assertEquals(rows.first(), opened) }
        capture("farther-helicopters.png")
    }

    @Test
    fun savedExpansionSurvivesLiveUpdatesAndScreenRestoration() {
        val restorer = StateRestorationTester(compose)
        val row = plane("FAR", 50.0, ObjectCategory.HELICOPTER)
        val uiState = mutableStateOf(state(listOf(row)))
        restorer.setContent { FriendOrFoeTheme { ListContent(uiState.value, ListActions()) } }
        compose.onNodeWithText("No detections within 10 mi").assertIsDisplayed()
        clickBranch("nearby_section_farther")
        clickBranch("nearby_type_farther_HELICOPTERS")
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_FAR"))
        compose.runOnIdle { uiState.value = state(listOf(row.copy(distanceMeters = 49 * AircraftRange.METERS_PER_MILE))) }
        compose.onNodeWithTag("list_row_FAR").assertIsDisplayed()
        restorer.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("list_row_FAR").assertIsDisplayed()
        clickBranch("nearby_section_farther")
        compose.onNodeWithTag("list_row_FAR").assertDoesNotExist()
    }

    @Test
    fun increasingRangeMovesExistingHelicopterIntoNearbyWithoutChangingDistanceOrder() {
        val rows = listOf(plane("HELI12", 12.0, ObjectCategory.HELICOPTER), plane("CLOSE1", 1.0))
        val uiState = mutableStateOf(state(rows).copy(groupAircraftByType = false))
        compose.setContent { FriendOrFoeTheme { ListContent(uiState.value, ListActions(onSetAircraftRangeMiles = { range ->
            uiState.value = uiState.value.copy(aircraftRangeMiles = range)
        })) } }
        compose.onNodeWithTag("list_row_HELI12").assertDoesNotExist()
        compose.onNodeWithTag("nearby_range").performClick()
        compose.onNodeWithTag("range_preset_15").performClick()
        compose.onNodeWithText("Done").performClick()
        val close = compose.onNodeWithTag("list_row_CLOSE1").fetchSemanticsNode().boundsInRoot
        val heli = compose.onNodeWithTag("list_row_HELI12").fetchSemanticsNode().boundsInRoot
        assertTrue(close.top < heli.top)
        compose.onNodeWithTag("nearby_section_farther").assertDoesNotExist()
        compose.onNodeWithTag("nearby_range").performClick()
        compose.onNodeWithTag("range_preset_10").performClick()
        compose.onNodeWithText("Done").performClick()
        compose.onNodeWithTag("list_row_HELI12").assertDoesNotExist()
    }

    @Test
    fun searchOpensDistantMatchesAndClearingSearchCollapsesThemAgain() {
        val row = plane("FAR50", 50.0, ObjectCategory.HELICOPTER)
        val uiState = mutableStateOf(state(listOf(row)))
        compose.setContent { FriendOrFoeTheme { ListContent(uiState.value, ListActions()) } }
        compose.runOnIdle { uiState.value = uiState.value.copy(filter = FilterState(searchQuery = "FAR50")) }
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_FAR50"))
        compose.onNodeWithTag("list_row_FAR50").assertIsDisplayed()
        compose.runOnIdle { uiState.value = uiState.value.copy(filter = FilterState()) }
        compose.onNodeWithTag("list_row_FAR50").assertDoesNotExist()
    }

    @Test
    fun flatViewKeepsRangeSectionsAndUnknownDistancesSeparate() {
        val rows = listOf(plane("FAR50", 50.0, ObjectCategory.HELICOPTER), plane("CLOSE1", 1.0), plane("UNKNOWN", null))
        val state = state(rows).copy(groupAircraftByType = false)
        compose.setContent { FriendOrFoeTheme { ListContent(state, ListActions()) } }
        compose.onNodeWithTag("list_row_CLOSE1").assertIsDisplayed()
        compose.onNodeWithTag("list_row_FAR50").assertDoesNotExist()
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_UNKNOWN"))
        compose.onNodeWithTag("list_distance_UNKNOWN", useUnmergedTree = true).assertTextEquals("Unknown")
        clickBranch("nearby_section_farther")
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_FAR50"))
        compose.onNodeWithTag("list_row_FAR50").assertIsDisplayed()
    }

    private fun clickBranch(key: String) {
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag(key))
        compose.onNodeWithTag(key).performClick()
    }

    private fun capture(name: String) {
        compose.mainClock.advanceTimeBy(600)
        compose.waitForIdle()
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, name).outputStream().use {
            image.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
    }

    private fun state(rows: List<SkyObject>) = ListUiState(
        body = ListBodyState.Results(rows), locationPermissionState = PermissionUiState.Granted,
    )

    private fun plane(id: String, miles: Double?, category: ObjectCategory = ObjectCategory.COMMERCIAL) = Aircraft(
        id = id, icaoHex = id, callsign = id, category = category, aircraftModel = if (category == ObjectCategory.HELICOPTER) "Bell 407" else "Boeing 737",
        position = Position(32.7, -117.1, 1000.0), firstSeen = Instant.now(), lastUpdated = Instant.now(),
        distanceMeters = miles?.times(AircraftRange.METERS_PER_MILE),
    )
}
