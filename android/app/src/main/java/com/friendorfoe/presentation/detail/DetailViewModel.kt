package com.friendorfoe.presentation.detail

import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.HistoryDao
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.time.Instant
import javax.inject.Inject
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * ViewModel for the Detail screen.
 *
 * Loads full detail for a specific sky object by ID.
 * For aircraft, fetches enrichment from the backend API.
 * For drones, uses locally available data from detection sources.
 */
@HiltViewModel
class DetailViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    private val aircraftRepository: AircraftRepository,
    private val historyDao: HistoryDao
) : ViewModel() {

    companion object {
        private const val TAG = "DetailViewModel"
    }

    private val _detailState = MutableStateFlow<DetailState>(DetailState.Idle)
    val detailState: StateFlow<DetailState> = _detailState.asStateFlow()

    private val _nearbyCandidates = MutableStateFlow<List<Drone>>(emptyList())
    val nearbyCandidates: StateFlow<List<Drone>> = _nearbyCandidates.asStateFlow()

    private val _positionTrail = MutableStateFlow<List<SkyObjectRepository.TrailPoint>>(emptyList())
    val positionTrail: StateFlow<List<SkyObjectRepository.TrailPoint>> = _positionTrail.asStateFlow()

    /**
     * Load detail for the given object ID.
     *
     * Looks up the object in the current sky objects list, then fetches
     * enrichment data from the backend if it's an aircraft.
     */
    fun loadDetail(objectId: String) {
        if (_detailState.value is DetailState.Loading) return

        _nearbyCandidates.value = emptyList()
        _positionTrail.value = emptyList()

        val skyObject = skyObjectRepository.skyObjects.value
            .firstOrNull { it.id == objectId }

        if (skyObject != null) {
            // Live object — show immediately, no Loading state
            loadFromLiveSkyObject(skyObject)
        } else {
            // Fallback: load from history database (for past detections)
            _detailState.value = DetailState.Loading
            loadFromHistory(objectId)
        }
    }

    private fun loadFromLiveSkyObject(skyObject: com.friendorfoe.domain.model.SkyObject) {
        when (skyObject) {
            is Aircraft -> {
                // Show data immediately — no spinner for live aircraft
                _detailState.value = DetailState.AircraftLoaded(
                    aircraft = skyObject,
                    detail = null
                )
                // Enrich asynchronously
                viewModelScope.launch {
                    val result = aircraftRepository.getAircraftDetail(skyObject.icaoHex)
                    result.fold(
                        onSuccess = { detail ->
                            _detailState.value = DetailState.AircraftLoaded(
                                aircraft = skyObject,
                                detail = detail
                            )
                        },
                        onFailure = { error ->
                            Log.w(TAG, "Failed to fetch aircraft detail for ${skyObject.icaoHex}: ${error.message}")
                            // Already showing aircraft data, no state change needed
                        }
                    )
                }
            }
            is Drone -> {
                _detailState.value = DetailState.DroneLoaded(drone = skyObject)
                viewModelScope.launch {
                    _nearbyCandidates.value = buildNearbyCandidates(skyObject)
                    _positionTrail.value = skyObjectRepository.getTrail(skyObject.id)
                }
            }
        }
    }

    private fun loadFromHistory(objectId: String) {
        viewModelScope.launch {
            try {
                val historyEntity = historyDao.getByObjectId(objectId)
                if (historyEntity == null) {
                    _detailState.value = DetailState.Error("Object not found.")
                    return@launch
                }

                val pos = Position(
                    latitude = historyEntity.latitude,
                    longitude = historyEntity.longitude,
                    altitudeMeters = historyEntity.altitudeMeters
                )
                val source = try {
                    DetectionSource.valueOf(historyEntity.detectionSource.uppercase())
                } catch (_: Exception) { DetectionSource.ADS_B }
                val category = try {
                    ObjectCategory.valueOf(historyEntity.category.uppercase())
                } catch (_: Exception) { ObjectCategory.UNKNOWN }
                val now = Instant.ofEpochMilli(historyEntity.lastSeen)

                if (historyEntity.objectType == "aircraft") {
                    val aircraft = Aircraft(
                        id = historyEntity.objectId,
                        position = pos,
                        source = source,
                        category = category,
                        confidence = historyEntity.confidence,
                        firstSeen = Instant.ofEpochMilli(historyEntity.firstSeen),
                        lastUpdated = now,
                        distanceMeters = historyEntity.distanceMeters,
                        icaoHex = historyEntity.objectId,
                        callsign = historyEntity.displayName,
                        photoUrl = historyEntity.photoUrl
                    )
                    _detailState.value = DetailState.AircraftLoaded(aircraft = aircraft, detail = null)
                } else {
                    val drone = Drone(
                        id = historyEntity.objectId,
                        position = pos,
                        source = source,
                        category = category,
                        confidence = historyEntity.confidence,
                        firstSeen = Instant.ofEpochMilli(historyEntity.firstSeen),
                        lastUpdated = now,
                        distanceMeters = historyEntity.distanceMeters,
                        droneId = historyEntity.objectId,
                        manufacturer = historyEntity.displayName
                    )
                    _detailState.value = DetailState.DroneLoaded(drone = drone)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to load from history: ${e.message}", e)
                _detailState.value = DetailState.Error("Could not load detail.")
            }
        }
    }

    private fun buildNearbyCandidates(tappedDrone: Drone): List<Drone> {
        val allDrones = skyObjectRepository.skyObjects.value.filterIsInstance<Drone>()
            .filter { it.id != tappedDrone.id }

        val tappedHasPos = tappedDrone.position.latitude != 0.0 || tappedDrone.position.longitude != 0.0

        return allDrones.sortedWith(compareBy<Drone> { candidate ->
            val candidateHasPos = candidate.position.latitude != 0.0 || candidate.position.longitude != 0.0
            if (tappedHasPos && candidateHasPos) 0 else 1
        }.thenBy { candidate ->
            val candidateHasPos = candidate.position.latitude != 0.0 || candidate.position.longitude != 0.0
            if (tappedHasPos && candidateHasPos) {
                haversineMeters(
                    tappedDrone.position.latitude, tappedDrone.position.longitude,
                    candidate.position.latitude, candidate.position.longitude
                )
            } else {
                // No position — sort by signal strength (stronger first → negate)
                -(candidate.signalStrengthDbm?.toDouble() ?: -999.0)
            }
        })
    }

    private fun haversineMeters(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val r = 6_371_000.0
        val dLat = Math.toRadians(lat2 - lat1)
        val dLon = Math.toRadians(lon2 - lon1)
        val a = sin(dLat / 2) * sin(dLat / 2) +
                cos(Math.toRadians(lat1)) * cos(Math.toRadians(lat2)) *
                sin(dLon / 2) * sin(dLon / 2)
        return r * 2 * atan2(sqrt(a), sqrt(1 - a))
    }
}

/** State for the detail view. */
sealed class DetailState {
    /** No object loaded */
    data object Idle : DetailState()

    /** Fetching detail from API */
    data object Loading : DetailState()

    /** Aircraft detail loaded (enrichment may be null if API failed) */
    data class AircraftLoaded(
        val aircraft: Aircraft,
        val detail: AircraftDetailDto?
    ) : DetailState()

    /** Drone detail loaded from local detection data */
    data class DroneLoaded(val drone: Drone) : DetailState()

    /** Error loading detail */
    data class Error(val message: String) : DetailState()
}
