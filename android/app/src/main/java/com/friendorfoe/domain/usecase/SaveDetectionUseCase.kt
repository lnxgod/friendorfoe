package com.friendorfoe.domain.usecase

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryRepository
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import javax.inject.Inject

/**
 * Use case for saving a detection to local history.
 *
 * Converts domain SkyObject instances to HistoryEntity for persistence.
 */
class SaveDetectionUseCase @Inject constructor(
    private val historyRepository: HistoryRepository
) {
    /**
     * Save a sky object detection to history.
     *
     * @param skyObject The detected object
     * @param userLatitude User's latitude at time of detection
     * @param userLongitude User's longitude at time of detection
     */
    suspend operator fun invoke(
        skyObject: SkyObject,
        userLatitude: Double,
        userLongitude: Double
    ) {
        val entity = when (skyObject) {
            is Aircraft -> HistoryEntity(
                objectId = skyObject.icaoHex,
                objectType = "aircraft",
                detectionSource = skyObject.source.name.lowercase(),
                category = skyObject.category.name.lowercase(),
                displayName = skyObject.callsign ?: skyObject.icaoHex,
                description = skyObject.displaySummary(),
                latitude = skyObject.position.latitude,
                longitude = skyObject.position.longitude,
                altitudeMeters = skyObject.position.altitudeMeters,
                userLatitude = userLatitude,
                userLongitude = userLongitude,
                distanceMeters = skyObject.distanceMeters,
                confidence = skyObject.confidence,
                firstSeen = skyObject.firstSeen.toEpochMilli(),
                lastSeen = skyObject.lastUpdated.toEpochMilli(),
                photoUrl = skyObject.photoUrl
            )
            is Drone -> HistoryEntity(
                objectId = skyObject.droneId,
                objectType = "drone",
                detectionSource = skyObject.source.name.lowercase(),
                category = skyObject.category.name.lowercase(),
                displayName = skyObject.manufacturer ?: "Unknown drone",
                description = skyObject.displaySummary(),
                latitude = skyObject.position.latitude,
                longitude = skyObject.position.longitude,
                altitudeMeters = skyObject.position.altitudeMeters,
                userLatitude = userLatitude,
                userLongitude = userLongitude,
                distanceMeters = skyObject.distanceMeters,
                confidence = skyObject.confidence,
                firstSeen = skyObject.firstSeen.toEpochMilli(),
                lastSeen = skyObject.lastUpdated.toEpochMilli()
            )
        }
        historyRepository.saveDetection(entity)
    }
}
