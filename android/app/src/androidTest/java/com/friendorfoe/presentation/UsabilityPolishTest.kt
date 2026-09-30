package com.friendorfoe.presentation

import android.graphics.Bitmap
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.aircraft.AircraftReferenceScreen
import com.friendorfoe.presentation.drones.DroneReferenceScreen
import com.friendorfoe.presentation.filter.CompactFilterBar
import com.friendorfoe.presentation.map.*
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import java.io.File
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class UsabilityPolishTest {
    @get:Rule val compose = createComposeRule()

    @Test fun clearingSearchPreservesOtherFiltersAndSearchKeyDismissesFocus() {
        var filter by mutableStateOf(FilterState(searchQuery = "N407", selectedCategories = setOf(ObjectCategory.HELICOPTER)))
        compose.setContent { FriendOrFoeTheme { Surface {
            CompactFilterBar(filter, 1, 2, { filter = filter.copy(searchQuery = it) }, {}, {})
        } } }
        compose.onNodeWithContentDescription("Clear search").performClick()
        compose.runOnIdle {
            assertEquals("", filter.searchQuery)
            assertEquals(setOf(ObjectCategory.HELICOPTER), filter.selectedCategories)
        }
        compose.onNodeWithTag("search_field").performTextInput("Bell")
        compose.onNodeWithTag("search_field").performImeAction()
        compose.onNodeWithTag("search_field").assertIsNotFocused()
        compose.runOnIdle { assertEquals("Bell", filter.searchQuery) }
    }

    @Test fun mapSearchCanHideWithoutSilentlyRemovingItsFilterAndDoneKeepsSelection() {
        var filter by mutableStateOf(FilterState())
        compose.setContent { FriendOrFoeTheme { Surface {
            MapWorkspaceControls(filter, 12) { filter = it }
        } } }
        compose.onNodeWithTag("search_field").assertDoesNotExist()
        val height = compose.onNodeWithTag("map_workspace_controls").fetchSemanticsNode().boundsInRoot.height
        assertTrue("Default toolbar should leave room for the map", height < with(compose.density) { 100.dp.toPx() })
        compose.onNodeWithContentDescription("Search map").performClick()
        compose.onNodeWithTag("search_field").performTextInput("N407")
        compose.onNodeWithContentDescription("Hide search").performClick()
        compose.onNodeWithTag("search_field").assertDoesNotExist()
        compose.onNodeWithText("1 filter active").assertIsDisplayed()
        compose.runOnIdle { assertEquals("N407", filter.searchQuery) }
        compose.onNodeWithTag("filter_clear").performClick()
        compose.onNodeWithText("1 filter active").assertDoesNotExist()
        compose.onNodeWithTag("filter_open").performClick()
        compose.onNodeWithText("Commercial").performClick()
        compose.onNodeWithTag("filter_done").performClick()
        compose.onNodeWithTag("filter_done").assertDoesNotExist()
        compose.runOnIdle { assertEquals(setOf(ObjectCategory.COMMERCIAL), filter.selectedCategories) }
        compose.onNodeWithText("1 filter active").assertIsDisplayed()
    }

    @Test fun mapControlsAndFilterActionsFitAtDoubleTextSize() {
        var filter by mutableStateOf(FilterState(searchQuery = "N407", selectedCategories = setOf(ObjectCategory.HELICOPTER)))
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, 2f)) {
                FriendOrFoeTheme { Surface(Modifier.width(360.dp).fillMaxHeight().testTag("polish_frame")) {
                    Column {
                        MapWorkspaceControls(filter, 1) { filter = it }
                        MapFlightTrailControls(FlightTrailWindow.FIFTEEN_MINUTES, MapFlightTrailsState(), {}, {}, {})
                    }
                } }
            }
        }
        val frame = compose.onNodeWithTag("polish_frame").fetchSemanticsNode().boundsInRoot
        listOf("map_search_toggle", "filter_open", "filter_clear", "flight_trails_menu", "fit_flight_trails").forEach { tag ->
            val bounds = compose.onNodeWithTag(tag).assertIsDisplayed().fetchSemanticsNode().boundsInRoot
            assertTrue("$tag outside viewport", bounds.left >= frame.left && bounds.right <= frame.right)
        }
        capture("map-controls-large-text.png")
        compose.onNodeWithTag("filter_open").performClick()
        compose.onNodeWithTag("filter_done").assertIsDisplayed().assertHasClickAction()
        compose.onNodeWithTag("filter_sheet_clear").assertIsDisplayed().performClick()
        compose.onNodeWithTag("filter_done").performClick()
        compose.runOnIdle { assertEquals(FilterState(), filter) }
    }

    @Test fun aircraftGuideRecoversFromNoMatches() {
        compose.setContent { FriendOrFoeTheme { AircraftReferenceScreen({}) } }
        compose.onNodeWithTag("search_field").performTextInput("no-such-airframe-xyz")
        compose.onNodeWithText("No matching aircraft").assertIsDisplayed()
        capture("reference-empty-dark.png")
        compose.onNodeWithTag("reference_reset").performClick()
        compose.onNodeWithTag("search_field").assert(SemanticsMatcher.expectValue(SemanticsProperties.EditableText, AnnotatedString("")))
        compose.onNodeWithTag("aircraft_reference_results").assertIsDisplayed()
    }

    @Test fun droneGuideResetClearsBothQueryAndCategory() {
        compose.setContent { FriendOrFoeTheme { DroneReferenceScreen({}) } }
        compose.onNode(hasText("Consumer") and isSelectable()).performClick()
        compose.onNodeWithTag("search_field").performTextInput("no-such-drone-xyz")
        compose.onNodeWithText("No matching drones").assertIsDisplayed()
        compose.onNodeWithTag("reference_reset").performClick()
        compose.onNodeWithText("All").assertIsSelected()
        compose.onNodeWithTag("search_field").assert(SemanticsMatcher.expectValue(SemanticsProperties.EditableText, AnnotatedString("")))
        compose.onNodeWithTag("drone_reference_results").assertIsDisplayed()
    }

    @Test fun referenceSearchAndExpansionSurviveStateRestoration() {
        val restoration = StateRestorationTester(compose)
        restoration.setContent { FriendOrFoeTheme { AircraftReferenceScreen({}, "FA18") } }
        compose.onNodeWithTag("search_field").performTextReplacement("hornet")
        compose.onNodeWithTag("reference_card_fa18").performClick()
        compose.onNodeWithTag("reference_card_fa18").assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription, "Expanded"))
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("search_field").assert(SemanticsMatcher.expectValue(SemanticsProperties.EditableText, AnnotatedString("hornet")))
        compose.onNodeWithTag("reference_card_fa18").assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription, "Expanded"))
    }

    @Test fun guideMetadataRemainsReadableAtDoubleTextSize() {
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, 2f)) {
                FriendOrFoeTheme { Surface(Modifier.width(360.dp)) { DroneReferenceScreen({}, "Hubsan") } }
            }
        }
        compose.onNodeWithTag("drone_reference_results").performScrollToNode(hasText("View details"))
        compose.onNodeWithText("View details", useUnmergedTree = true).assertIsDisplayed()
        capture("reference-large-text.png")
        compose.onNodeWithTag("reference_card_hubsan").performClick()
        compose.onNodeWithText("Hide details", useUnmergedTree = true).performScrollTo().assertIsDisplayed()
        capture("reference-expanded-large-text.png")
    }

    private fun capture(name: String) {
        compose.mainClock.advanceTimeBy(600)
        compose.waitForIdle()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, "usability-$name")
            .outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
    }
}
