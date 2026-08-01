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
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class HistoryViewModel @Inject constructor(
    private val historyStore: HistoryStore,
) : ViewModel() {
    private val filter = MutableStateFlow(FilterState())
    private val pendingDeletion = MutableStateFlow<PendingHistoryDeletion?>(null)
    private val deletionError = MutableStateFlow<String?>(null)
    private val deletionInProgress = MutableStateFlow(false)

    val uiState: StateFlow<HistoryUiState> = combine(
        historyStore.observeAll(),
        filter,
        pendingDeletion,
        deletionError,
        deletionInProgress,
    ) { entries, currentFilter, pending, currentDeletionError, isDeleting ->
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
            deletionError = currentDeletionError,
            deletionInProgress = isDeleting,
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
        if (deletionInProgress.value) return
        deletionError.value = null
        pendingDeletion.value = PendingHistoryDeletion.Row(row.id, row.displayName)
    }

    fun requestClearAll() {
        if (deletionInProgress.value) return
        deletionError.value = null
        pendingDeletion.value = PendingHistoryDeletion.All
    }

    fun dismissDeletion() {
        if (deletionInProgress.value) return
        pendingDeletion.value = null
        deletionError.value = null
    }

    fun confirmDeletion() = pendingDeletion.value
        ?.takeUnless { deletionInProgress.value }
        ?.let { pending ->
            deletionInProgress.value = true
            deletionError.value = null
            viewModelScope.launch {
                try {
                    when (pending) {
                        is PendingHistoryDeletion.Row -> historyStore.deleteById(pending.id)
                        PendingHistoryDeletion.All -> historyStore.clearAll()
                    }
                    pendingDeletion.value = null
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Throwable) {
                    deletionError.value = when (pending) {
                        is PendingHistoryDeletion.Row -> "Couldn't delete this detection. Try again."
                        PendingHistoryDeletion.All -> "Couldn't clear history. Try again."
                    }
                } finally {
                    deletionInProgress.value = false
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
