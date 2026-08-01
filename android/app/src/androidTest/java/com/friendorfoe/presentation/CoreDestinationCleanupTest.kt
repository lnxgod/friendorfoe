package com.friendorfoe.presentation

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.components.CollectionBodyState
import com.friendorfoe.presentation.history.HISTORY_RETENTION_COPY
import com.friendorfoe.presentation.history.HistoryActions
import com.friendorfoe.presentation.history.HistoryContent
import com.friendorfoe.presentation.history.HistoryUiState
import com.friendorfoe.presentation.list.ListActions
import com.friendorfoe.presentation.list.ListBodyState
import com.friendorfoe.presentation.list.ListContent
import com.friendorfoe.presentation.list.ListDestinationContent
import com.friendorfoe.presentation.list.ListUiState
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import java.time.Instant
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class CoreDestinationCleanupTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun historyEmptyUsesTruthfulRetentionCopyWithoutExportOrOverflow() {
        setHistory(HistoryUiState(body = CollectionBodyState.Empty))

        compose.onNodeWithText("History").assertIsDisplayed()
        compose.onNodeWithText("No saved detections").assertIsDisplayed()
        compose.onNodeWithText(HISTORY_RETENTION_COPY).assertIsDisplayed()
        compose.onNodeWithText("Export", substring = true).assertDoesNotExist()
        compose.onNodeWithContentDescription("More options").assertDoesNotExist()
    }

    @Test
    fun activeDistanceFilterExplainsUnknownDistancePolicy() {
        compose.setContent {
            FriendOrFoeTheme {
                HistoryContent(
                    state = HistoryUiState(
                        filter = FilterState(maxDistanceNm = 5f, isAdvancedExpanded = true),
                        activeFilterCount = 1,
                        body = CollectionBodyState.NoMatches(activeFilterCount = 1),
                    ),
                    onFilterChanged = {},
                    onEntryTapped = {},
                    onRequestDelete = {},
                    onRequestClearAll = {},
                    onDismissDeletion = {},
                    onConfirmDeletion = {},
                )
            }
        }

        compose.onNodeWithText("Rows without distance are excluded").assertIsDisplayed()
    }

    @Test
    fun historyFailureRetryCallsTheResubscribeAction() {
        var retryCalls = 0
        setHistory(
            state = HistoryUiState(
                body = CollectionBodyState.Failed(
                    message = "Couldn't load History. Try again.",
                    canRetry = true,
                ),
            ),
            actions = HistoryActions(onRetry = { retryCalls += 1 }),
        )

        compose.onNodeWithText("Couldn't load History. Try again.").assertIsDisplayed()
        compose.onNodeWithText("Retry").performClick()

        assertTrue(retryCalls == 1)
    }

    @Test
    fun historyRowsExposeHumanSourceAndCategoryLabels() {
        setHistory(
            HistoryUiState(
                totalCount = 1,
                body = CollectionBodyState.Content(
                    listOf(history(detectionSource = "wifi_nan", category = "general_aviation")),
                ),
            ),
        )

        compose.onNodeWithText("Remote ID · Wi-Fi").assertIsDisplayed()
        compose.onNodeWithText("General aviation").assertIsDisplayed()
    }

    @Test
    fun listResultsExposeHumanEvidenceAndOpenObjectPeekCallback() {
        val row = aircraft(id = "N123", source = DetectionSource.ADS_B)
        val remoteId = drone(id = "RID", source = DetectionSource.REMOTE_ID)
        val phone = drone(id = "PHONE", source = DetectionSource.WIFI)
        val military = aircraft(
            id = "MIL",
            source = DetectionSource.WIFI_BEACON,
            category = ObjectCategory.MILITARY,
        )
        var opened: SkyObject? = null
        setList(
            body = ListBodyState.Results(listOf(row, remoteId, phone, military)),
            actions = ListActions(onOpenPeek = { opened = it }),
        )

        compose.onNodeWithTag("list_results").assertIsDisplayed()
        compose.onNodeWithText("List").assertIsDisplayed()
        compose.onNodeWithText("ADS-B").assertIsDisplayed()
        compose.onNodeWithText("Remote ID").assertIsDisplayed()
        compose.onNodeWithText("Remote ID · Wi-Fi").assertIsDisplayed()
        compose.onNodeWithText("Phone").assertIsDisplayed()
        compose.onNodeWithText("Commercial").assertIsDisplayed()
        compose.onNodeWithText("Military").assertIsDisplayed()
        compose.onNodeWithText("Badge status", substring = true).assertDoesNotExist()
        compose.onNodeWithText("Privacy", substring = true).assertDoesNotExist()
        compose.onNodeWithText("About", substring = true).assertDoesNotExist()
        compose.onNodeWithText("Reference Guide", substring = true).assertDoesNotExist()

        compose.onNodeWithTag("list_row_N123").performClick()
        assertSame(row, opened)
    }

    @Test
    fun listRowOpensObjectPeekBeforeFullDetails() {
        val row = aircraft(id = "PEEK", source = DetectionSource.ADS_B)
        var fullDetailsId: String? = null
        compose.setContent {
            FriendOrFoeTheme {
                ListDestinationContent(
                    state = ListUiState(body = ListBodyState.Results(listOf(row))),
                    actions = ListActions(),
                    onFullDetails = { fullDetailsId = it },
                )
            }
        }

        compose.onNodeWithTag("list_row_PEEK").performClick()

        compose.onNodeWithText("ADS-B radio match").assertIsDisplayed()
        compose.onNodeWithText("Capture").assertIsNotEnabled()
        compose.onNodeWithText("Full details").performClick()
        assertTrue(fullDetailsId == "PEEK")
    }

    @Test
    fun listLoadingIsDistinctFromAnEmptyResolvedFeed() {
        setList(ListBodyState.Loading)

        compose.onNodeWithText("Loading nearby detections").assertIsDisplayed()
        compose.onNodeWithText("0 objects").assertDoesNotExist()
        compose.onNodeWithText("0 results").assertDoesNotExist()
    }

    @Test
    fun listNoDetectionsExplainsWhatWillAppear() {
        setList(ListBodyState.NoDetections)

        compose.onNodeWithText("No nearby detections").assertIsDisplayed()
        compose.onNodeWithText(
            "Aircraft and drones will appear here when detected nearby.",
        ).assertIsDisplayed()
    }

    @Test
    fun listNoMatchesOffersClearFilters() {
        var clearCalls = 0
        setList(
            body = ListBodyState.NoMatches(activeFilterCount = 2),
            filter = FilterState(searchQuery = "none"),
            activeFilterCount = 2,
            actions = ListActions(onClearFilters = { clearCalls += 1 }),
        )

        compose.onNodeWithText("No matches for 2 active filters").assertIsDisplayed()
        compose.onNodeWithTag("filter_clear").performClick()
        assertTrue(clearCalls == 1)
    }

    @Test
    fun listStaleResultsKeepRowsAndShowTextualWarning() {
        setList(
            ListBodyState.StaleResults(
                rows = listOf(aircraft("CACHED", DetectionSource.ADS_B)),
                ageMs = null,
                message = "ADS-B is temporarily unavailable",
            ),
        )

        compose.onNodeWithTag("list_results").assertIsDisplayed()
        compose.onNodeWithText("STALE").assertIsDisplayed()
        compose.onNodeWithText("ADS-B is temporarily unavailable").assertIsDisplayed()
        compose.onNodeWithText("Saved result age is unavailable").assertIsDisplayed()
    }

    @Test
    fun listFailureDoesNotPretendTheFeedIsEmpty() {
        setList(ListBodyState.Failed("Nearby sources are unavailable"))

        compose.onNodeWithText("Nearby sources are unavailable").assertIsDisplayed()
        compose.onNodeWithText("No nearby detections").assertDoesNotExist()
        compose.onNodeWithText("0 objects").assertDoesNotExist()
        compose.onNodeWithText("0 results").assertDoesNotExist()
    }

    @Test
    fun compactListKeepsFilterAndRowTargetsAtLeastFortyEightDp() {
        compose.setContent {
            val currentDensity = LocalDensity.current
            CompositionLocalProvider(
                LocalDensity provides Density(currentDensity.density, 1.3f),
            ) {
                FriendOrFoeTheme {
                    Box(Modifier.width(360.dp).height(700.dp)) {
                        ListContent(
                            state = ListUiState(
                                body = ListBodyState.Results(
                                    listOf(aircraft("COMPACT", DetectionSource.ADS_B)),
                                ),
                            ),
                            actions = ListActions(),
                        )
                    }
                }
            }
        }

        listOf("filter_open", "list_row_COMPACT").forEach { tag ->
            val bounds = compose.onNodeWithTag(tag)
                .assertIsDisplayed()
                .fetchSemanticsNode().boundsInRoot
            assertTrue(bounds.height >= with(compose.density) { 48.dp.toPx() })
            assertTrue(bounds.left >= 0f)
            assertTrue(bounds.right <= with(compose.density) { 360.dp.toPx() })
        }
    }

    private fun setHistory(
        state: HistoryUiState,
        actions: HistoryActions = HistoryActions(),
    ) {
        compose.setContent {
            FriendOrFoeTheme {
                HistoryContent(state = state, actions = actions)
            }
        }
    }

    private fun setList(
        body: ListBodyState,
        filter: FilterState = FilterState(),
        activeFilterCount: Int = 0,
        actions: ListActions = ListActions(),
    ) {
        compose.setContent {
            FriendOrFoeTheme {
                ListContent(
                    state = ListUiState(
                        filter = filter,
                        activeFilterCount = activeFilterCount,
                        body = body,
                    ),
                    actions = actions,
                )
            }
        }
    }
}

private fun aircraft(
    id: String,
    source: DetectionSource,
    category: ObjectCategory = ObjectCategory.COMMERCIAL,
) = Aircraft(
    id = id,
    position = Position(32.7, -117.1, 1_000.0),
    source = source,
    category = category,
    confidence = 0.95f,
    firstSeen = Instant.now(),
    lastUpdated = Instant.now(),
    distanceMeters = 1_500.0,
    icaoHex = id,
    callsign = id,
    aircraftModel = "Test aircraft",
)

private fun drone(id: String, source: DetectionSource) = Drone(
    id = id,
    position = Position(32.7, -117.1, 100.0),
    source = source,
    confidence = 0.9f,
    firstSeen = Instant.now(),
    lastUpdated = Instant.now(),
    distanceMeters = 100.0,
    droneId = id,
    manufacturer = "Test",
)

private fun history(
    detectionSource: String,
    category: String,
) = HistoryEntity(
    id = 1L,
    objectId = "history-object",
    objectType = "aircraft",
    detectionSource = detectionSource,
    category = category,
    displayName = "Stored aircraft",
    description = "Stored description",
    latitude = 32.7,
    longitude = -117.1,
    altitudeMeters = 1_000.0,
    userLatitude = 32.8,
    userLongitude = -117.2,
    distanceMeters = 2_000.0,
    confidence = 0.9f,
    firstSeen = 1_700_000_000_000L,
    lastSeen = 1_700_000_001_000L,
)
