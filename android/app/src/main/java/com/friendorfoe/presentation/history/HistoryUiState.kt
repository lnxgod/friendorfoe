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
)

sealed interface PendingHistoryDeletion {
    data class Row(val id: Long, val label: String) : PendingHistoryDeletion
    data object All : PendingHistoryDeletion
}
