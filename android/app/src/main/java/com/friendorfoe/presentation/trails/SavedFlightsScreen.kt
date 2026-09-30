package com.friendorfoe.presentation.trails

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.local.SavedFlightSummary
import com.friendorfoe.data.repository.SavedFlightRepository
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

@HiltViewModel
class SavedFlightsViewModel @Inject constructor(private val repository: SavedFlightRepository) : ViewModel() {
    private val reload = MutableStateFlow(0)
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    val flights = reload.flatMapLatest {
        repository.observeAll().map<List<SavedFlightSummary>, Result<List<SavedFlightSummary>>> { Result.success(it) }
            .catch { if (it is CancellationException) throw it else emit(Result.failure(it)) }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)
    val deleteError = MutableStateFlow<String?>(null)
    fun retry() { reload.value++ }
    fun delete(id: String) { viewModelScope.launch {
        try { repository.delete(id); deleteError.value = null
        } catch (e: CancellationException) { throw e
        } catch (_: Exception) { deleteError.value = "Could not delete the saved flight. Try again." }
    } }
}

@Composable
fun SavedFlightsScreen(onBack: () -> Unit, onOpen: (String) -> Unit, viewModel: SavedFlightsViewModel = hiltViewModel()) {
    val result by viewModel.flights.collectAsStateWithLifecycle()
    val error by viewModel.deleteError.collectAsStateWithLifecycle()
    var pendingDelete by remember { mutableStateOf<SavedFlightSummary?>(null) }
    Column(Modifier.fillMaxSize()) {
        FofSecondaryScreenHeader("Saved flights", onBack)
        Text("Saved copies stay on this phone until deleted. They include aircraft locations.",
            modifier = Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium)
        error?.let { Text(it, modifier = Modifier.padding(16.dp), color = MaterialTheme.colorScheme.error) }
        when {
            result == null -> CircularProgressIndicator(Modifier.padding(16.dp))
            result?.isFailure == true -> TextButton(onClick = viewModel::retry) { Text("Couldn't load saved flights · Retry") }
            result?.getOrNull().isNullOrEmpty() -> Text("Open a flight path and choose Save this flight.", Modifier.padding(16.dp))
            else -> LazyColumn {
                items(result?.getOrNull().orEmpty(), key = { it.id }) { flight ->
                    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Column(Modifier.weight(1f).clickable { onOpen(flight.id) }.padding(vertical = 16.dp)) {
                            Text(flight.label, style = MaterialTheme.typography.titleMedium)
                            Text("${flight.pointCount} positions · ${formatTrackTime(flight.savedAt)}", style = MaterialTheme.typography.bodySmall)
                        }
                        TextButton(onClick = { pendingDelete = flight }) { Text("Delete") }
                    }
                    HorizontalDivider()
                }
            }
        }
    }
    pendingDelete?.let { flight ->
        AlertDialog(onDismissRequest = { pendingDelete = null }, title = { Text("Delete saved flight?") },
            text = { Text("Remove the saved copy of ${flight.label} from this phone?") },
            confirmButton = { TextButton(onClick = { viewModel.delete(flight.id); pendingDelete = null }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { pendingDelete = null }) { Text("Cancel") } })
    }
}
