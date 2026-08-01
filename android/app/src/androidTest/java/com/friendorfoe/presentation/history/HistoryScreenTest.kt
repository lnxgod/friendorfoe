package com.friendorfoe.presentation.history

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.presentation.components.CollectionBodyState
import com.friendorfoe.presentation.detail.HistoricalDetailContent
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class HistoryScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun tappingRowPassesImmutableDatabaseId() {
        var selected: Long? = null
        compose.setContent {
            FriendOrFoeTheme {
                HistoryContent(
                    state = contentState(
                        history(id = 11L, objectId = "same"),
                        history(id = 12L, objectId = "same"),
                    ),
                    onFilterChanged = {},
                    onEntryTapped = { selected = it },
                    onRequestDelete = {},
                    onRequestClearAll = {},
                    onDismissDeletion = {},
                    onConfirmDeletion = {},
                )
            }
        }

        compose.onNodeWithTag("history_row_11").performClick()

        assertEquals(11L, selected)
    }

    @Test
    fun historicalPresentationUsesStoredFieldsAndOmitsLiveStatus() {
        compose.setContent {
            FriendOrFoeTheme {
                HistoricalDetailContent(
                    snapshot = history(
                        id = 11L,
                        displayName = "Stored callsign",
                        description = "Stored description",
                        detectionSource = "remote_id",
                        latitude = 12.34567,
                        longitude = -76.54321,
                        distanceMeters = 987.0,
                        confidence = 0.73f,
                    ),
                )
            }
        }

        compose.onNodeWithText("Historical detection").assertIsDisplayed()
        compose.onNodeWithText("Stored callsign").assertIsDisplayed()
        compose.onNodeWithText("Stored description").assertIsDisplayed()
        compose.onNodeWithText("Remote ID").assertIsDisplayed()
        compose.onNodeWithText("12.34567, -76.54321").assertIsDisplayed()
        compose.onNodeWithText("73%").assertIsDisplayed()
        compose.onNodeWithText("Now").assertDoesNotExist()
        compose.onNodeWithText("Current Distance").assertDoesNotExist()
        compose.onNodeWithText("Live").assertDoesNotExist()
    }

    @Test
    fun rowAndClearAllUseDistinctConfirmationsAndCancelHasNoOperation() {
        val row = history(id = 11L, displayName = "Stored callsign")
        var state by mutableStateOf(contentState(row))
        var deletedId: Long? = null
        var rowDeleteCalls by mutableIntStateOf(0)
        var clearCalls by mutableIntStateOf(0)
        compose.setContent {
            FriendOrFoeTheme {
                HistoryContent(
                    state = state,
                    onFilterChanged = {},
                    onEntryTapped = {},
                    onRequestDelete = {
                        state = state.copy(
                            pendingDeletion = PendingHistoryDeletion.Row(it.id, it.displayName),
                        )
                    },
                    onRequestClearAll = {
                        state = state.copy(pendingDeletion = PendingHistoryDeletion.All)
                    },
                    onDismissDeletion = { state = state.copy(pendingDeletion = null) },
                    onConfirmDeletion = {
                        when (val pending = state.pendingDeletion) {
                            is PendingHistoryDeletion.Row -> {
                                deletedId = pending.id
                                rowDeleteCalls++
                            }
                            PendingHistoryDeletion.All -> clearCalls++
                            null -> Unit
                        }
                        state = state.copy(pendingDeletion = null)
                    },
                )
            }
        }

        compose.onNodeWithTag("history_delete_11").performClick()
        compose.onNodeWithText("Delete Stored callsign?").assertIsDisplayed()
        compose.onNodeWithTag("history_cancel_delete").performClick()
        assertEquals(0, rowDeleteCalls)
        assertEquals(0, clearCalls)

        compose.onNodeWithTag("history_delete_11").performClick()
        compose.onNodeWithTag("history_confirm_delete_row").performClick()
        assertEquals(11L, deletedId)
        assertEquals(1, rowDeleteCalls)
        assertEquals(0, clearCalls)

        compose.onNodeWithTag("history_clear_all").performClick()
        compose.onNodeWithText("Clear all history?").assertIsDisplayed()
        compose.onNodeWithTag("history_cancel_delete").performClick()
        assertEquals(0, clearCalls)

        compose.onNodeWithTag("history_clear_all").performClick()
        compose.onNodeWithTag("history_confirm_clear_all").performClick()
        assertEquals(1, rowDeleteCalls)
        assertEquals(1, clearCalls)
    }
}

private fun contentState(vararg rows: HistoryEntity) = HistoryUiState(
    totalCount = rows.size,
    body = CollectionBodyState.Content(rows.toList()),
)

private fun history(
    id: Long = 1L,
    objectId: String = "history-object",
    objectType: String = "aircraft",
    detectionSource: String = "ads_b",
    category: String = "commercial",
    displayName: String = "TEST123",
    description: String? = "Stored description",
    latitude: Double = 37.6213,
    longitude: Double = -122.3790,
    altitudeMeters: Double = 1_234.0,
    userLatitude: Double = 37.7749,
    userLongitude: Double = -122.4194,
    distanceMeters: Double? = 2_500.0,
    confidence: Float = 0.91f,
    firstSeen: Long = 1_699_999_900_000L,
    lastSeen: Long = 1_700_000_000_000L,
    photoUrl: String? = "https://example.test/stored.jpg",
) = HistoryEntity(
    id = id,
    objectId = objectId,
    objectType = objectType,
    detectionSource = detectionSource,
    category = category,
    displayName = displayName,
    description = description,
    latitude = latitude,
    longitude = longitude,
    altitudeMeters = altitudeMeters,
    userLatitude = userLatitude,
    userLongitude = userLongitude,
    distanceMeters = distanceMeters,
    confidence = confidence,
    firstSeen = firstSeen,
    lastSeen = lastSeen,
    photoUrl = photoUrl,
)
