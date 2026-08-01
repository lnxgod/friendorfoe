package com.friendorfoe.presentation.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

@Composable
fun FofLaunchPlaceholder() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        CircularProgressIndicator(Modifier.semantics { contentDescription = "Loading app" })
    }
}

@Composable
fun FofLoadingState(message: String) = FofMessageState(message, showProgress = true)

@Composable
fun FofEmptyState(message: String) = FofMessageState(message)

@Composable
fun FofNoMatchesState(activeFilterCount: Int, onClearFilters: () -> Unit) {
    FofMessageState(
        message = "No matches for $activeFilterCount active filters",
        actionLabel = "Clear filters",
        onAction = onClearFilters,
    )
}

@Composable
fun FofFailureState(message: String, onRetry: (() -> Unit)? = null) {
    FofMessageState(message, actionLabel = onRetry?.let { "Retry" }, onAction = onRetry)
}

@Composable
fun FofErrorState(
    title: String,
    detail: String,
    actionLabel: String,
    onAction: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        Text(detail, style = MaterialTheme.typography.bodyMedium)
        TextButton(onClick = onAction, modifier = Modifier.heightIn(min = 48.dp)) {
            Text(actionLabel)
        }
    }
}

sealed interface CollectionBodyState<out T> {
    data object Loading : CollectionBodyState<Nothing>
    data class Content<T>(val rows: List<T>) : CollectionBodyState<T>
    data class Stale<T>(val rows: List<T>, val ageMs: Long, val message: String) : CollectionBodyState<T>
    data object Empty : CollectionBodyState<Nothing>
    data class NoMatches(val activeFilterCount: Int) : CollectionBodyState<Nothing>
    data class Failed(val message: String, val canRetry: Boolean) : CollectionBodyState<Nothing>
}

@Composable
private fun FofMessageState(
    message: String,
    showProgress: Boolean = false,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
) {
    Column(
        Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (showProgress) CircularProgressIndicator()
        Text(message, style = MaterialTheme.typography.bodyMedium)
        if (actionLabel != null && onAction != null) {
            TextButton(onClick = onAction, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(actionLabel)
            }
        }
    }
}
