package com.friendorfoe.presentation.detail

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class DetailOverviewContentTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun historicalDetailIsClearlySavedAndRawFieldsStartCollapsed() {
        compose.setContent {
            FriendOrFoeTheme {
                DetailOverviewContent(
                    model = model(isLive = false),
                )
            }
        }

        compose.onNodeWithText("SAVED").assertIsDisplayed()
        compose.onNodeWithText("Historical detection").assertIsDisplayed()
        compose.onNodeWithText("Immutable snapshot from History").assertIsDisplayed()
        compose.onNodeWithText("History record").assertDoesNotExist()
        compose.onNodeWithTag("detail_aircraft_photo").assertDoesNotExist()

        compose.onNodeWithTag("detail_raw").performClick()
        compose.onNodeWithText("History record").assertIsDisplayed()
        compose.onNodeWithText("11").assertIsDisplayed()
    }

    @Test
    fun partialLiveDetailKeepsSummaryAndOffersRetry() {
        var retries = 0
        compose.setContent {
            FriendOrFoeTheme {
                DetailOverviewContent(
                    model = model(isLive = true).copy(
                        supportingMessage = "Aircraft details are unavailable. Local detection details are still shown.",
                        retryLabel = "Retry details",
                    ),
                    onRetryDetails = { retries += 1 },
                )
            }
        }

        compose.onNodeWithText("LIVE").assertIsDisplayed()
        compose.onNodeWithText("ADS-B").assertIsDisplayed()
        compose.onNodeWithText("Retry details").assertIsDisplayed()
        compose.onNodeWithTag("detail_retry").performClick()
        compose.runOnIdle { assertEquals(1, retries) }
    }

    @Test
    fun knownAircraftTypeLoadsItsBundledPhoto() {
        compose.setContent {
            FriendOrFoeTheme {
                DetailOverviewContent(
                    model = model(isLive = true).copy(
                        aircraftVisual = AircraftVisual(
                            photoUrl = null,
                            typeCode = "B738",
                            description = "Boeing 737-800",
                            category = com.friendorfoe.domain.model.ObjectCategory.COMMERCIAL,
                        ),
                    ),
                )
            }
        }

        compose.onNodeWithTag("detail_aircraft_photo").assertIsDisplayed()
        compose.waitUntil(timeoutMillis = 5_000) {
            compose.onAllNodesWithTag(
                "detail_aircraft_photo_image",
                useUnmergedTree = true,
            ).fetchSemanticsNodes().isNotEmpty()
        }
        compose.onNodeWithTag(
            "detail_aircraft_photo_image",
            useUnmergedTree = true,
        ).assertIsDisplayed()
    }

    @Test
    fun unknownAircraftTypeKeepsAVisibleSilhouette() {
        compose.setContent {
            FriendOrFoeTheme {
                DetailOverviewContent(
                    model = model(isLive = true).copy(
                        aircraftVisual = AircraftVisual(
                            photoUrl = null,
                            typeCode = "ZZZZ",
                            description = null,
                            category = com.friendorfoe.domain.model.ObjectCategory.COMMERCIAL,
                        ),
                    ),
                )
            }
        }

        compose.onNodeWithTag(
            "detail_aircraft_silhouette",
            useUnmergedTree = true,
        ).assertIsDisplayed()
        compose.onNodeWithTag(
            "detail_aircraft_photo_image",
            useUnmergedTree = true,
        ).assertDoesNotExist()
    }

    private fun model(isLive: Boolean) = DetailPresentation(
        title = "FOF42",
        statusLabel = if (isLive) "Live detection" else "Historical detection",
        isLive = isLive,
        summary = listOf(
            DetailField("Source", "ADS-B"),
            DetailField("Category", "Commercial"),
        ),
        identifiers = listOf(
            DetailIdentifier("ICAO address", "abc123", copyable = true),
        ),
        advanced = listOf(DetailField("Altitude", "4,921 ft")),
        raw = listOf(DetailField("History record", "11")),
        retryLabel = null,
    )
}
