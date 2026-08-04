package com.friendorfoe.presentation.map

import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.model.isFormationDroneId
import com.friendorfoe.sensor.TrajectoryPredictor

data class MapTrack(
    val skyObject: SkyObject,
    val position: Position,
    val ageSeconds: Float,
    val confidence: Float,
    val isExtrapolated: Boolean,
    val headingDegrees: Float?,
)

/** A stationary formation pixel that never needs frame-by-frame projection. */
internal data class FormationMapPoint(
    val objectId: String,
    val latitude: Double,
    val longitude: Double,
)

internal data class MapProjectionSources(
    val trackObjects: List<SkyObject>,
    val formationPoints: List<FormationMapPoint>,
)

/**
 * Removes C5 formation pixels before the animated trajectory clock. Their
 * coordinates change only when a new Remote ID observation is published, so
 * projecting them every map frame creates work without changing the drawing.
 */
internal fun splitMapProjectionSources(objects: List<SkyObject>): MapProjectionSources {
    val trackObjects = ArrayList<SkyObject>(objects.size)
    val formationPoints = ArrayList<FormationMapPoint>()
    objects.forEach { obj ->
        val drone = obj as? Drone
        if (drone != null && isFormationDroneId(drone.droneId)) {
            formationPoints += FormationMapPoint(
                objectId = obj.id,
                latitude = obj.position.latitude,
                longitude = obj.position.longitude,
            )
        } else {
            trackObjects += obj
        }
    }
    return MapProjectionSources(
        trackObjects = trackObjects,
        formationPoints = formationPoints,
    )
}

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
