package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.detection.PrivacyCategory
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val repository: PrivacyFindingRepository,
) : ViewModel() {
    private val filters = MutableStateFlow(PrivacyFilterState())

    val uiState: StateFlow<PrivacyUiState> = combine(
        repository.currentState,
        filters,
    ) { current, activeFilters ->
        projectPrivacyUiState(current, activeFilters)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = projectPrivacyUiState(repository.currentState.value),
    )

    fun updateQuery(query: String) {
        filters.value = filters.value.copy(query = query)
    }

    fun toggleCategory(category: PrivacyCategory) {
        filters.value = filters.value.copy(
            categories = filters.value.categories.toggle(category),
        )
    }

    fun toggleSource(source: PrivacySourceKind) {
        filters.value = filters.value.copy(
            sources = filters.value.sources.toggle(source),
        )
    }

    fun clearFilters() {
        filters.value = PrivacyFilterState()
    }

    fun ignore(finding: PrivacyFinding) {
        if (!finding.capabilities.canIgnore) return
        viewModelScope.launch {
            repository.ignore(finding.observationKey)
        }
    }

    fun recover(source: PrivacySourceKind) {
        viewModelScope.launch {
            repository.recover(source)
        }
    }

    fun retryAllFailed() {
        viewModelScope.launch {
            repository.retryAllFailed()
        }
    }

    private fun <T> Set<T>.toggle(value: T): Set<T> =
        if (value in this) this - value else this + value
}
