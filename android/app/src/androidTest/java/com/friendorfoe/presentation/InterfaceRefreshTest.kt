package com.friendorfoe.presentation

import androidx.compose.ui.graphics.asAndroidBitmap
import android.graphics.Bitmap
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.test.espresso.Espresso.pressBack
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.AircraftRange
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.presentation.about.AboutLandingActions
import com.friendorfoe.presentation.about.AboutLandingScreen
import com.friendorfoe.presentation.list.*
import com.friendorfoe.presentation.navigation.*
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import java.io.File
import java.time.Instant
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class InterfaceRefreshTest {
    @get:Rule val compose = createComposeRule()

    @Test fun rangeCanBeChangedFromNearbyAndAppliesToExistingRows() {
        var miles by mutableIntStateOf(10)
        var openedSettings = false
        val rows = listOf(aircraft("CLOSE", 1.0), aircraft("HELI", 12.0, ObjectCategory.HELICOPTER))
        compose.setContent {
            FriendOrFoeTheme {
                Surface {
                    ListContent(
                        ListUiState(body = ListBodyState.Results(sortSkyObjectsForList(rows, emptySet(), miles)),
                            locationPermissionState = PermissionUiState.Granted, aircraftRangeMiles = miles),
                        ListActions(onSetAircraftRangeMiles = { miles = it }, onOpenSettings = { openedSettings = true }),
                    )
                }
            }
        }
        compose.onNodeWithTag("nearby_range").performClick()
        compose.onNodeWithTag("aircraft_range_slider").performSemanticsAction(SemanticsActions.SetProgress) { it(15f) }
        compose.runOnIdle { assertEquals(15, miles) }
        compose.onNodeWithText("Done").performClick()
        val helicopter = compose.onNodeWithTag("list_row_HELI").fetchSemanticsNode().boundsInRoot
        val close = compose.onNodeWithTag("list_row_CLOSE").fetchSemanticsNode().boundsInRoot
        assertTrue(helicopter.top < close.top)
        compose.onNodeWithTag("nearby_range").assertTextContains("15 mi range").performClick()
        compose.onNodeWithTag("aircraft_range_reset").performClick()
        compose.runOnIdle { assertEquals(10, miles) }
        compose.onNodeWithText("Notification settings").performClick()
        compose.runOnIdle { assertTrue(openedSettings) }
    }

    @Test fun moreOpensHistoryAndBadgeAndBackReturnsToMore() {
        compose.setContent {
            FriendOrFoeTheme {
                val nav = rememberNavController()
                NavHost(nav, startDestination = Screen.About.route) {
                    composable(Screen.About.route) { AboutTopLevelRoute(nav) }
                    composable(Screen.History.route) { Text("Saved observations", Modifier.testTag("history_target")) }
                    composable(Screen.Badge.route) { Text("Scanner controls", Modifier.testTag("badge_target")) }
                }
            }
        }
        compose.onNodeWithTag("more_history").performClick()
        compose.onNodeWithTag("history_target").assertIsDisplayed()
        pressBack()
        compose.onNodeWithTag("more_badge").performClick()
        compose.onNodeWithTag("badge_target").assertIsDisplayed()
        pressBack()
        compose.onNodeWithTag("about_landing").assertIsDisplayed()
    }

    @Test fun nearbyDarkShowsDistanceAndRetainsDistantHelicopter() = renderNearby(dark = true)
    @Test fun nearbyLightShowsDistanceAndRetainsDistantHelicopter() = renderNearby(dark = false)
    @Test fun fiveTabsRemainLabeledAndReachableAtDoubleTextSize() = renderNearby(dark = true, fontScale = 2f)

    private fun renderNearby(dark: Boolean, fontScale: Float = 1f) {
        var selected by mutableStateOf(TopLevelDestination.LIST)
        val rows = listOf(
            aircraft("N407PD", 2.4, ObjectCategory.HELICOPTER, "Bell 407"),
            aircraft("N172SP", 3.1, ObjectCategory.GENERAL_AVIATION, "Cessna 172"),
            aircraft("UAL842", 6.8, ObjectCategory.COMMERCIAL, "Boeing 737"),
            aircraft("N128ER", 18.2, ObjectCategory.HELICOPTER, "Airbus H125"),
            aircraft("SWA216", 24.7, ObjectCategory.COMMERCIAL, "Boeing 737"),
        )
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, fontScale)) {
                FriendOrFoeTheme(darkTheme = dark) {
                    Box(Modifier.width(360.dp).fillMaxHeight().testTag("refresh_frame")) {
                        FofNavigationSuite(true, selected.route, { selected = it }) {
                            ListContent(ListUiState(body = ListBodyState.Results(rows),
                                locationPermissionState = PermissionUiState.Granted), ListActions())
                        }
                    }
                }
            }
        }
        val frame = compose.onNodeWithTag("refresh_frame").fetchSemanticsNode().boundsInRoot
        compose.onNodeWithTag("list_distance_N407PD", useUnmergedTree = true).assertTextEquals("2.4 mi")
        compose.onNodeWithTag("list_results").performScrollToNode(hasTestTag("list_row_N128ER"))
        compose.onNodeWithTag("list_distance_N128ER", useUnmergedTree = true).assertTextEquals("18.2 mi")
        compose.onNodeWithTag("list_results").performScrollToIndex(0)
        capture("nearby-${if (dark) "dark" else "light"}${if (fontScale > 1f) "-large-text" else ""}.png")
        primaryDestinations.forEach { tab ->
            val node = compose.onNodeWithContentDescription(tab.label).assertIsDisplayed().assertHasClickAction()
            val bounds = node.fetchSemanticsNode().boundsInRoot
            assertTrue(bounds.left >= frame.left && bounds.right <= frame.right)
            assertTrue(bounds.height >= with(compose.density) { 48.dp.toPx() })
            node.performClick()
            compose.runOnIdle { assertEquals(tab, selected) }
        }
        compose.onNodeWithContentDescription("Nearby").performClick()
    }

    @Test fun moreUsesReadableUnboxedActions() {
        compose.setContent { FriendOrFoeTheme(darkTheme = true) {
            FofNavigationSuite(true, Screen.About.route, {}) {
                AboutLandingScreen(AboutLandingActions(), installedVersionName = "0.67.22")
            }
        } }
        compose.onNodeWithTag("more_history").assertIsDisplayed()
        capture("more-dark.png")
    }

    private fun capture(name: String) {
        compose.mainClock.advanceTimeBy(600)
        compose.waitForIdle()
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(instrumentation.targetContext.filesDir, "ui-refresh-$name").outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        bitmap.recycle()
    }

    private fun aircraft(id: String, miles: Double, category: ObjectCategory = ObjectCategory.GENERAL_AVIATION,
        model: String = "Test aircraft") = Aircraft(
        id = id, icaoHex = id, callsign = id, aircraftModel = model,
        category = category, position = Position(32.7, -117.1, 1000.0),
        distanceMeters = miles * AircraftRange.METERS_PER_MILE,
        firstSeen = Instant.now(), lastUpdated = Instant.now(),
    )
}
