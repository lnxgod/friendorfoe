package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class IgnoredFindingRow(
    val encodedKey: String,
    val sourceLabel: String,
    val stableId: String,
)

internal fun ignoredFindingRows(encodedKeys: Set<String>): List<IgnoredFindingRow> =
    encodedKeys.mapNotNull { encoded ->
        val key = FindingPreferenceKey.decode(encoded) ?: return@mapNotNull null
        val source = PrivacySourceKind.entries.singleOrNull {
            it.preferenceId == key.source
        }
        IgnoredFindingRow(
            encodedKey = encoded,
            sourceLabel = source?.userLabel() ?: key.source,
            stableId = key.stableId,
        )
    }.sortedWith(compareBy(IgnoredFindingRow::sourceLabel, IgnoredFindingRow::stableId))

@HiltViewModel
class IgnoredDevicesViewModel @Inject constructor(
    appPreferences: AppPreferences,
    private val repository: PrivacyFindingRepository,
) : ViewModel() {
    val rows: StateFlow<List<IgnoredFindingRow>> = appPreferences.ignoredFindingKeys
        .map(::ignoredFindingRows)
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5_000),
            initialValue = emptyList(),
        )

    fun restore(encodedKey: String) {
        viewModelScope.launch {
            repository.restore(encodedKey)
        }
    }
}

@Composable
fun IgnoredDevicesScreen(
    onBack: () -> Unit,
    viewModel: IgnoredDevicesViewModel = hiltViewModel(),
) {
    val rows by viewModel.rows.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize()) {
        FofSecondaryScreenHeader("Ignored findings", onBack)
        if (rows.isEmpty()) {
            FofEmptyState(
                title = "Nothing ignored",
                detail = "Findings you ignore from Privacy will appear here.",
                modifier = Modifier.fillMaxSize().padding(24.dp),
            )
        } else {
            LazyColumn(Modifier.fillMaxSize()) {
                items(rows, key = IgnoredFindingRow::encodedKey) { row ->
                    IgnoredFindingRowContent(row, viewModel::restore)
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                }
            }
        }
    }
}

@Composable
private fun IgnoredFindingRowContent(
    row: IgnoredFindingRow,
    onRestore: (String) -> Unit,
) {
    androidx.compose.foundation.layout.Row(
        modifier = Modifier.fillMaxWidth().padding(
            start = 16.dp,
            top = 8.dp,
            end = 8.dp,
            bottom = 8.dp,
        ),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = row.sourceLabel,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = row.stableId,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        TextButton(
            onClick = { onRestore(row.encodedKey) },
            modifier = Modifier.heightIn(min = 48.dp),
        ) {
            Text("Restore")
        }
    }
}
