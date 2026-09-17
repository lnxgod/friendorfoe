package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import com.friendorfoe.presentation.components.FofLoadingState
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import com.friendorfoe.presentation.navigation.Screen
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn

@HiltViewModel
class PrivacyFindingDetailsViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    repository: PrivacyFindingRepository,
) : ViewModel() {
    private val key = Screen.PrivacyFinding.keyFromNavigationArguments(
        source = savedStateHandle["source"],
        record = savedStateHandle["record"],
    )
    val encounter = repository.encounters.map { entries -> entries.firstOrNull { it.key == key } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)
    val state: StateFlow<PrivacyFindingLookupState> =
        (key?.let(repository::finding) ?: flowOf(PrivacyFindingLookupState.Expired))
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.Eagerly,
                initialValue = if (key == null) {
                    PrivacyFindingLookupState.Expired
                } else {
                    PrivacyFindingLookupState.Loading
                },
            )
}

@Composable
fun PrivacyFindingDetailsRoute(
    onBack: () -> Unit,
    onBackToPrivacy: () -> Unit,
    viewModel: PrivacyFindingDetailsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val encounter by viewModel.encounter.collectAsStateWithLifecycle()
    PrivacyFindingDetailsContent(state, onBack, onBackToPrivacy, encounter)
}

@Composable
fun PrivacyFindingDetailsContent(
    state: PrivacyFindingLookupState,
    onBack: () -> Unit,
    onBackToPrivacy: () -> Unit,
    encounter: PrivacyEncounter? = null,
) {
    Column(Modifier.fillMaxSize()) {
        FofSecondaryScreenHeader("Privacy finding", onBack)
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            when (state) {
                PrivacyFindingLookupState.Loading -> FofLoadingState("Loading current finding")
                PrivacyFindingLookupState.Expired -> Column(
                    modifier = Modifier.fillMaxWidth().padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Text("Item no longer current", style = MaterialTheme.typography.titleLarge)
                    Text(
                        "This exact finding has expired or is no longer in the current list.",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    TextButton(
                        onClick = onBackToPrivacy,
                        modifier = Modifier.heightIn(min = 48.dp),
                    ) { Text("Back to Privacy") }
                }
                is PrivacyFindingLookupState.Present -> FindingDetails(state.finding)
            }
            if (state == PrivacyFindingLookupState.Expired && encounter != null) {
                Text("Saved observation", Modifier.padding(horizontal = 20.dp), style = MaterialTheme.typography.titleMedium)
                FindingDetails(encounter.finding.copy(freshness = FindingFreshness.EXPIRED, capabilities = PrivacyCapabilities()))
            }
            encounter?.let { EncounterEvidence(it) }
        }
    }
}

@Composable
private fun FindingDetails(finding: PrivacyFinding) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(finding.title, style = MaterialTheme.typography.headlineSmall)
        Text(
            "${finding.severity.detailLabel()} · ${finding.source.userLabel()} · " +
                finding.freshness.detailLabel(),
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.SemiBold,
            color = MaterialTheme.colorScheme.primary,
        )
        if (finding.ownership == Ownership.OWNED) Text("Your device")
        finding.evidence?.let { Text(it) }
        finding.limitation?.let {
            Text(it, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        finding.signalDbm?.let { Text("Signal: $it dBm") }
    }
}

private fun FindingSeverity.detailLabel(): String = when (this) {
    FindingSeverity.CRITICAL -> "Threat"
    FindingSeverity.AWARENESS -> "Awareness"
    FindingSeverity.NEARBY -> "Nearby"
    FindingSeverity.INFO -> "Info"
}

private fun FindingFreshness.detailLabel(): String = when (this) {
    FindingFreshness.LIVE -> "Live"
    FindingFreshness.STALE -> "Stale"
    FindingFreshness.PAUSED_CACHED -> "Paused copy"
    FindingFreshness.EXPIRED -> "Expired"
}
