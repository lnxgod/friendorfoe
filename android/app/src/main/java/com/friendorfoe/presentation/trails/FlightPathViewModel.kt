package com.friendorfoe.presentation.trails

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.AircraftTrackRepository
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.data.repository.SavedFlightRepository
import com.friendorfoe.data.repository.decodedPoints
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
    val isSaved: Boolean = false,
    val saveMessage: String? = null,
    val saving: Boolean = false,
)

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
@HiltViewModel
class FlightPathViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    tracks: AircraftTrackRepository,
    history: HistoryStore,
    skyObjects: SkyObjectRepository,
    clock: MonotonicClock,
    private val savedFlights: SavedFlightRepository,
) : ViewModel() {
    private val savedId = savedStateHandle.get<String>("savedId")
    private val saveStatus = MutableStateFlow<Pair<Boolean, String?>>(false to null)
    private val objectId = savedStateHandle.get<String>("objectId").orEmpty()
    private val savedLabel = MutableStateFlow(objectId)
    private val reload = MutableStateFlow(0)
    private data class Loaded(val label: String?, val points: List<TrackingEntity>)
    private val points = reload.flatMapLatest {
        val source = if (savedId != null) savedFlights.observe(savedId).map {
            checkNotNull(it) { "Saved flight no longer exists" }
            Loaded(it.label, it.decodedPoints())
        } else tracks.observeTrail(objectId).map { Loaded(null, it) }
        source.map<Loaded, Result<Loaded>> { Result.success(it) }.catch { error ->
            if (error is CancellationException) throw error
            emit(Result.failure(error))
        }
    }
    val state = combine(points, skyObjects.skyObjects, savedLabel, clock.ticks(60_000), saveStatus) { result, objects, label, _, save ->
        val live = objects.filterIsInstance<Aircraft>().firstOrNull { it.id == objectId }
        FlightPathState(
            label = result.getOrNull()?.label ?: live?.callsign ?: live?.registration ?: label,
            points = result.getOrNull()?.points.orEmpty(),
            nowMs = clock.nowWallClock().toEpochMilli(),
            loading = false,
            error = if (result.isFailure) "Could not load the recorded flight path." else null,
            isSaved = savedId != null,
            saving = save.first,
            saveMessage = save.second,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), FlightPathState(label = objectId))

    fun save(points: List<TrackingEntity>) {
        if (savedId != null || saveStatus.value.first) return
        saveStatus.value = true to null
        viewModelScope.launch {
            try {
                savedFlights.save(objectId, state.value.label, points)
                saveStatus.value = false to "Saved in More → Saved flights. Kept until you delete it."
            } catch (error: CancellationException) { throw error
            } catch (_: Exception) { saveStatus.value = false to "Could not save this flight. Try again." }
        }
    }

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
