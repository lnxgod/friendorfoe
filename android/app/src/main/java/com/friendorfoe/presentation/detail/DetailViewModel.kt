package com.friendorfoe.presentation.detail

import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

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
    private val aircraftRepository: AircraftRepository
) : ViewModel() {

    companion object {
        private const val TAG = "DetailViewModel"
    }

    private val _detailState = MutableStateFlow<DetailState>(DetailState.Idle)
    val detailState: StateFlow<DetailState> = _detailState.asStateFlow()

    /**
     * Load detail for the given object ID.
     *
     * Looks up the object in the current sky objects list, then fetches
     * enrichment data from the backend if it's an aircraft.
     */
    fun loadDetail(objectId: String) {
        if (_detailState.value is DetailState.Loading) return

        _detailState.value = DetailState.Loading

        val skyObject = skyObjectRepository.skyObjects.value
            .firstOrNull { it.id == objectId }

        if (skyObject == null) {
            _detailState.value = DetailState.Error("Object not found. It may have moved out of range.")
            return
        }

        viewModelScope.launch {
            when (skyObject) {
                is Aircraft -> {
                    val result = aircraftRepository.getAircraftDetail(skyObject.icaoHex)
                    result.fold(
                        onSuccess = { detail ->
                            _detailState.value = DetailState.AircraftLoaded(
                                aircraft = skyObject,
                                detail = detail
                            )
                        },
                        onFailure = { error ->
                            // Show aircraft with local data even if enrichment API fails
                            _detailState.value = DetailState.AircraftLoaded(
                                aircraft = skyObject,
                                detail = null
                            )
                            Log.w(TAG, "Failed to fetch aircraft detail for ${skyObject.icaoHex}: ${error.message}")
                        }
                    )
                }
                is Drone -> {
                    _detailState.value = DetailState.DroneLoaded(drone = skyObject)
                }
            }
        }
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
