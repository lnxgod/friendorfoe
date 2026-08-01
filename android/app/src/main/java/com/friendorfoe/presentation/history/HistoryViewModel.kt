package com.friendorfoe.presentation.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.domain.usecase.FilterEngine
import com.friendorfoe.presentation.components.CollectionBodyState
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.map
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
    private val historyReloads = MutableSharedFlow<Unit>(replay = 1).apply { tryEmit(Unit) }
    private val historyRows = historyReloads.flatMapLatest {
        historyStore.observeAll()
            .map<List<HistoryEntity>, Result<List<HistoryEntity>>> { Result.success(it) }
            .catch { error ->
                if (error is CancellationException) throw error
                emit(Result.failure(error))
            }
    }

    val uiState: StateFlow<HistoryUiState> = combine(
        historyRows,
        filter,
        pendingDeletion,
        deletionError,
        deletionInProgress,
    ) { rowsResult, currentFilter, pending, currentDeletionError, isDeleting ->
        val entries = rowsResult.getOrNull()
        val filterCount = activeFilterCount(currentFilter)
        val filtered = entries?.let { FilterEngine.applyFilters(it, currentFilter) }.orEmpty()
        HistoryUiState(
            filter = currentFilter,
            totalCount = entries?.size ?: 0,
            activeFilterCount = filterCount,
            body = when {
                entries == null -> CollectionBodyState.Failed(
                    message = "Couldn't load History. Try again.",
                    canRetry = true,
                )
                entries.isEmpty() -> CollectionBodyState.Empty
                filtered.isEmpty() -> CollectionBodyState.NoMatches(
                    activeFilterCount = filterCount,
                )
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

    fun retryHistory() {
        historyReloads.tryEmit(Unit)
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
