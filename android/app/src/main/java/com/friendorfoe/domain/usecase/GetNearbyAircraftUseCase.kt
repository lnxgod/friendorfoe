package com.friendorfoe.domain.usecase

import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.domain.model.Aircraft
import javax.inject.Inject

/**
 * Use case for fetching nearby aircraft.
 *
 * Encapsulates the business logic for querying aircraft within
 * range of the user's current position via the backend API.
 */
class GetNearbyAircraftUseCase @Inject constructor(
    private val aircraftRepository: AircraftRepository
) {
    /**
     * Execute the use case.
     *
     * @param latitude User's current latitude
     * @param longitude User's current longitude
     * @param radiusNm Search radius in nautical miles
     * @return Result containing list of Aircraft on success
     */
    suspend operator fun invoke(
        latitude: Double,
        longitude: Double,
        radiusNm: Int = 50
    ): Result<List<Aircraft>> {
        return aircraftRepository.getNearbyAircraft(latitude, longitude, radiusNm)
    }
}
