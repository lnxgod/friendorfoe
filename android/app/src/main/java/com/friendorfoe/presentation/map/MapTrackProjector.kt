package com.friendorfoe.presentation.map

import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.sensor.TrajectoryPredictor

data class MapTrack(
    val skyObject: SkyObject,
    val position: Position,
    val ageSeconds: Float,
    val confidence: Float,
    val isExtrapolated: Boolean,
    val headingDegrees: Float?,
)

class MapTrackProjector(
    private val predictor: TrajectoryPredictor = TrajectoryPredictor(),
) {
    fun project(objects: List<SkyObject>, nowMs: Long): List<MapTrack> =
        predictor.predictAll(objects, nowMs).map { predicted ->
            MapTrack(
                skyObject = predicted.skyObject,
                position = predicted.predictedPosition,
                ageSeconds = predicted.ageSeconds,
                confidence = predicted.confidence,
                isExtrapolated = predicted.isExtrapolated,
                headingDegrees = predicted.trackHeadingDegrees,
            )
        }
}
