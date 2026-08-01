package com.friendorfoe.presentation.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.usecase.FilterEngine
import com.friendorfoe.presentation.components.CollectionBodyState
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class HistoryViewModel @Inject constructor(
    private val historyStore: HistoryStore,
) : ViewModel() {
    private val filter = MutableStateFlow(FilterState())
    private val pendingDeletion = MutableStateFlow<PendingHistoryDeletion?>(null)

    val uiState: StateFlow<HistoryUiState> = combine(
        historyStore.observeAll(),
        filter,
        pendingDeletion,
    ) { entries, currentFilter, pending ->
        val filtered = FilterEngine.applyFilters(entries, currentFilter)
        HistoryUiState(
            filter = currentFilter,
            totalCount = entries.size,
            activeFilterCount = currentFilter.activeCount(),
            body = when {
                entries.isEmpty() -> CollectionBodyState.Empty
                filtered.isEmpty() -> CollectionBodyState.Empty
                else -> CollectionBodyState.Content(filtered)
            },
            pendingDeletion = pending,
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.Eagerly,
        initialValue = HistoryUiState(),
    )

    fun updateFilter(filterState: FilterState) {
        filter.value = filterState
    }

    fun requestDelete(row: HistoryEntity) {
        pendingDeletion.value = PendingHistoryDeletion.Row(row.id, row.displayName)
    }

    fun requestClearAll() {
        pendingDeletion.value = PendingHistoryDeletion.All
    }

    fun dismissDeletion() {
        pendingDeletion.value = null
    }

    fun confirmDeletion() = pendingDeletion.value?.let { pending ->
        pendingDeletion.value = null
        viewModelScope.launch {
            when (pending) {
                is PendingHistoryDeletion.Row -> historyStore.deleteById(pending.id)
                PendingHistoryDeletion.All -> historyStore.clearAll()
            }
        }
    } ?: viewModelScope.launch { }
}

private fun FilterState.activeCount(): Int =
    selectedCategories.size +
        selectedSources.size +
        listOfNotNull(objectTypeFilter, maxDistanceNm, minAltitudeFt, maxAltitudeFt).size +
        searchQuery.takeIf(String::isNotBlank)?.let { 1 }.orZero()

private fun Int?.orZero(): Int = this ?: 0
