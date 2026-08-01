package com.friendorfoe.presentation.history

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.presentation.components.CollectionBodyState

data class HistoryUiState(
    val filter: FilterState = FilterState(),
    val totalCount: Int = 0,
    val activeFilterCount: Int = 0,
    val body: CollectionBodyState<HistoryEntity> = CollectionBodyState.Loading,
    val pendingDeletion: PendingHistoryDeletion? = null,
    val deletionError: String? = null,
    val deletionInProgress: Boolean = false,
)

data class HistoryActions(
    val onQueryChanged: (String) -> Unit = {},
    val onOpenFilters: () -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onOpenRow: (Long) -> Unit = {},
    val onRequestDelete: (HistoryEntity) -> Unit = {},
    val onRequestClearAll: () -> Unit = {},
    val onRetry: () -> Unit = {},
    val onDismissDeletion: () -> Unit = {},
    val onConfirmDeletion: () -> Unit = {},
)

sealed interface PendingHistoryDeletion {
    data class Row(val id: Long, val label: String) : PendingHistoryDeletion
    data object All : PendingHistoryDeletion
}
