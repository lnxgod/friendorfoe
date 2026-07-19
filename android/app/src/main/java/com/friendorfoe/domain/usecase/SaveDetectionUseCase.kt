package com.friendorfoe.domain.usecase

import com.friendorfoe.data.local.toHistoryEntity
import com.friendorfoe.data.repository.HistoryRepository
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
        val entity = skyObject.toHistoryEntity(userLatitude, userLongitude)
        historyRepository.saveDetection(entity)
    }
}
