package com.friendorfoe.data.repository

import com.friendorfoe.data.remote.AircraftDto
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.data.remote.FriendOrFoeApiService
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Repository for aircraft data.
 *
 * Fetches aircraft data from the backend API and maps DTOs to domain models.
 * Future: add caching layer for offline support.
 */
@Singleton
class AircraftRepository @Inject constructor(
    private val apiService: FriendOrFoeApiService
) {

    /**
     * Fetch nearby aircraft from the backend.
     *
     * @param latitude User's current latitude
     * @param longitude User's current longitude
     * @param radiusNm Search radius in nautical miles
     * @return List of Aircraft domain objects
     */
    suspend fun getNearbyAircraft(
        latitude: Double,
        longitude: Double,
        radiusNm: Int = 50
    ): Result<List<Aircraft>> {
        return try {
            val dtos = apiService.getNearbyAircraft(latitude, longitude, radiusNm)
            val aircraft = dtos.map { it.toDomain() }
            Result.success(aircraft)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    /**
     * Fetch detailed info for a specific aircraft.
     */
    suspend fun getAircraftDetail(icaoHex: String): Result<AircraftDetailDto> {
        return try {
            val detail = apiService.getAircraftDetail(icaoHex)
            Result.success(detail)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    private fun AircraftDto.toDomain(): Aircraft {
        val now = Instant.now()
        return Aircraft(
            id = icaoHex,
            position = Position(
                latitude = latitude,
                longitude = longitude,
                altitudeMeters = altitudeMeters,
                heading = heading,
                speedMps = speedMps
            ),
            category = mapCategory(category),
            firstSeen = now,
            lastUpdated = Instant.ofEpochSecond(lastContact),
            icaoHex = icaoHex,
            callsign = callsign?.trim(),
            registration = registration,
            aircraftType = aircraftType,
            squawk = squawk,
            isOnGround = onGround
        )
    }

    private fun mapCategory(category: String?): ObjectCategory {
        return when (category?.lowercase()) {
            "commercial", "airline" -> ObjectCategory.COMMERCIAL
            "general_aviation", "private" -> ObjectCategory.GENERAL_AVIATION
            "military" -> ObjectCategory.MILITARY
            else -> ObjectCategory.UNKNOWN
        }
    }
}
