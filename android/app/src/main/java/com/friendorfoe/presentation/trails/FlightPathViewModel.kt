package com.friendorfoe.presentation.trails

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.AircraftTrackRepository
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.domain.model.Aircraft
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

data class FlightPathState(
    val label: String = "Aircraft",
    val points: List<TrackingEntity> = emptyList(),
    val nowMs: Long = System.currentTimeMillis(),
    val loading: Boolean = true,
    val error: String? = null,
)

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
@HiltViewModel
class FlightPathViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    tracks: AircraftTrackRepository,
    history: HistoryStore,
    skyObjects: SkyObjectRepository,
    clock: MonotonicClock,
) : ViewModel() {
    private val objectId = savedStateHandle.get<String>("objectId").orEmpty()
    private val savedLabel = MutableStateFlow(objectId)
    private val reload = MutableStateFlow(0)
    private val points = reload.flatMapLatest {
        tracks.observeTrail(objectId).map<List<TrackingEntity>, Result<List<TrackingEntity>>> { Result.success(it) }
            .catch { error ->
                if (error is CancellationException) throw error
                emit(Result.failure(error))
            }
    }
    val state = combine(points, skyObjects.skyObjects, savedLabel, clock.ticks(60_000)) { result, objects, label, _ ->
        val live = objects.filterIsInstance<Aircraft>().firstOrNull { it.id == objectId }
        FlightPathState(
            label = live?.callsign ?: live?.registration ?: label,
            points = result.getOrDefault(emptyList()),
            nowMs = clock.nowWallClock().toEpochMilli(),
            loading = false,
            error = if (result.isFailure) "Could not load the recorded flight path." else null,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), FlightPathState(label = objectId))

    init {
        viewModelScope.launch {
            try {
                history.getNewestByObjectId(objectId)?.let { savedLabel.value = it.displayName }
            } catch (error: CancellationException) {
                throw error
            } catch (_: Exception) {
                // The stable identifier still labels a path when history lookup is unavailable.
            }
        }
    }

    fun retry() { reload.value += 1 }
}
