package com.friendorfoe.sensor

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Predicted position for a sky object, including extrapolation metadata.
 *
 * @property skyObject The original sky object (unmodified)
 * @property predictedPosition Extrapolated lat/lon/alt based on heading + speed
 * @property ageSeconds Seconds since last ADS-B/detection update
 * @property confidence 1.0 when fresh, decays linearly toward 0 at [STALE_THRESHOLD_S]
 * @property isExtrapolated True if any forward projection was applied
 * @property trackHeadingDegrees Current predicted heading (may include turn rate), null if unknown
 */
data class PredictedObject(
    val skyObject: SkyObject,
    val predictedPosition: Position,
    val ageSeconds: Float,
    val confidence: Float,
    val isExtrapolated: Boolean,
    val trackHeadingDegrees: Float?
)

/**
 * Dead-reckoning position predictor for sky objects.
 *
 * Extrapolates aircraft positions forward in time between ADS-B polls using
 * reported heading, ground speed, and vertical rate. Smoothly blends predicted
 * positions when new ADS-B data arrives to avoid label jumps.
 *
 * Key features:
 * - Spherical forward projection (accurate at all latitudes)
 * - Blend offset with exponential decay on new data (no visual discontinuity)
 * - Turn rate detection from heading deltas between consecutive reports
 * - Confidence decay as data ages
 */
class TrajectoryPredictor {

    companion object {
        private const val EARTH_RADIUS_M = 6_371_000.0

        /** Max seconds to extrapolate forward — beyond this, prediction error grows too large */
        const val MAX_EXTRAPOLATION_S = 12.0

        /** Beyond this age, confidence = 0 and no prediction is attempted */
        const val STALE_THRESHOLD_S = 30.0

        /** Blend offset half-life in ms — controls how fast the label glides to corrected position */
        private const val BLEND_DECAY_MS = 700.0

        /** Below this speed (m/s), treat as stationary */
        private const val MIN_SPEED_MPS = 1.0f

        /** Below this turn rate (deg/s), don't apply heading rotation */
        private const val MIN_TURN_RATE_DPS = 0.5f
    }

    /**
     * Per-track state for each sky object.
     */
    internal class TrackState(
        var reportPosition: Position,
        var reportTimeMs: Long,
        var headingDeg: Float?,
        var speedMps: Float?,
        var verticalRateMps: Float?,
        var turnRateDps: Float = 0f,
        // Blend offset: difference between old prediction and new report, decayed over time
        var blendOffsetNorthM: Double = 0.0,
        var blendOffsetEastM: Double = 0.0,
        var blendOffsetUpM: Double = 0.0,
        var blendStartMs: Long = 0L,
        var lastSeenReportMs: Long = Long.MIN_VALUE
    )

    private val tracks = mutableMapOf<String, TrackState>()

    /**
     * Predict positions for all sky objects at the given time.
     *
     * @param objects Current list of sky objects from repository
     * @param nowMs Current time in epoch milliseconds
     * @return List of [PredictedObject] with extrapolated positions and metadata
     */
    fun predictAll(objects: List<SkyObject>, nowMs: Long): List<PredictedObject> {
        // Clean up tracks for objects no longer in the list
        val activeIds = objects.map { it.id }.toSet()
        tracks.keys.removeAll { it !in activeIds }

        return objects.map { obj -> predictSingle(obj, nowMs) }
    }

    private fun predictSingle(obj: SkyObject, nowMs: Long): PredictedObject {
        val reportMs = obj.lastUpdated.toEpochMilli()
        val ageSeconds = ((nowMs - reportMs).coerceAtLeast(0L)) / 1000.0f
        val confidence = (1.0f - (ageSeconds / STALE_THRESHOLD_S.toFloat())).coerceIn(0f, 1f)

        // Skip extrapolation for non-aircraft, on-ground aircraft, or stale data
        val isAircraft = obj is Aircraft
        val isOnGround = (obj as? Aircraft)?.isOnGround == true
        val pos = obj.position
        val heading = pos.heading
        val speed = pos.speedMps

        if (!isAircraft || isOnGround || heading == null || speed == null
            || speed < MIN_SPEED_MPS || ageSeconds > STALE_THRESHOLD_S.toFloat()
        ) {
            return PredictedObject(
                skyObject = obj,
                predictedPosition = pos,
                ageSeconds = ageSeconds,
                confidence = confidence,
                isExtrapolated = false,
                trackHeadingDegrees = heading
            )
        }

        // Get or create track state
        val track = tracks.getOrPut(obj.id) {
            TrackState(
                reportPosition = pos,
                reportTimeMs = reportMs,
                headingDeg = heading,
                speedMps = speed,
                verticalRateMps = pos.verticalRateMps,
                lastSeenReportMs = reportMs
            )
        }

        // Ingest new report if timestamp advanced
        if (reportMs > track.lastSeenReportMs) {
            ingestNewReport(track, obj, nowMs)
        }

        // Predict forward from report position
        val predicted = predict(track, nowMs)

        return PredictedObject(
            skyObject = obj,
            predictedPosition = predicted,
            ageSeconds = ageSeconds,
            confidence = confidence,
            isExtrapolated = true,
            trackHeadingDegrees = track.headingDeg?.let {
                val dt = ((nowMs - track.reportTimeMs).coerceIn(0L, (MAX_EXTRAPOLATION_S * 1000).toLong())) / 1000.0
                if (abs(track.turnRateDps) > MIN_TURN_RATE_DPS) {
                    AngleUtils.normalizeAngle360(it + track.turnRateDps * dt.toFloat())
                } else it
            }
        )
    }

    /**
     * Handle arrival of new ADS-B data. Computes blend offset so the label
     * glides smoothly from old predicted position to new trajectory.
     */
    private fun ingestNewReport(track: TrackState, obj: SkyObject, nowMs: Long) {
        val pos = obj.position
        val reportMs = obj.lastUpdated.toEpochMilli()

        // Compute where we THOUGHT the object was at nowMs (old prediction)
        val oldPredicted = predict(track, nowMs)

        // Detect turn rate from heading change
        val newHeading = pos.heading
        val oldHeading = track.headingDeg
        if (newHeading != null && oldHeading != null) {
            val dtReport = ((reportMs - track.reportTimeMs).coerceAtLeast(1000L)) / 1000.0
            val headingDelta = AngleUtils.normalizeAngleDifference(newHeading - oldHeading)
            val rawTurnRate = (headingDelta / dtReport).toFloat()
            // Low-pass filter turn rate to avoid noise
            track.turnRateDps = track.turnRateDps * 0.5f + rawTurnRate * 0.5f
        }

        // Update track with new report
        track.reportPosition = pos
        track.reportTimeMs = reportMs
        track.headingDeg = pos.heading
        track.speedMps = pos.speedMps
        track.verticalRateMps = pos.verticalRateMps
        track.lastSeenReportMs = reportMs

        // Compute where the NEW trajectory says the object is at nowMs
        val newPredicted = predictRaw(track, nowMs)

        // Blend offset = old predicted - new predicted (decays to zero)
        val meanLat = Math.toRadians((oldPredicted.latitude + newPredicted.latitude) / 2.0)
        track.blendOffsetNorthM = (oldPredicted.latitude - newPredicted.latitude) * Math.PI / 180.0 * EARTH_RADIUS_M
        track.blendOffsetEastM = (oldPredicted.longitude - newPredicted.longitude) * Math.PI / 180.0 * EARTH_RADIUS_M * cos(meanLat)
        track.blendOffsetUpM = oldPredicted.altitudeMeters - newPredicted.altitudeMeters
        track.blendStartMs = nowMs
    }

    /**
     * Predict position at [nowMs] from track state, including blend offset.
     */
    private fun predict(track: TrackState, nowMs: Long): Position {
        val raw = predictRaw(track, nowMs)

        // Apply decaying blend offset
        val blendAge = (nowMs - track.blendStartMs).coerceAtLeast(0L)
        val blendFactor = exp(-blendAge / BLEND_DECAY_MS)

        if (blendFactor < 0.01) return raw // negligible, skip math

        val meanLat = Math.toRadians(raw.latitude)
        val offsetLatDeg = (track.blendOffsetNorthM * blendFactor) / EARTH_RADIUS_M * 180.0 / Math.PI
        val offsetLonDeg = (track.blendOffsetEastM * blendFactor) / (EARTH_RADIUS_M * cos(meanLat)) * 180.0 / Math.PI
        val offsetAlt = track.blendOffsetUpM * blendFactor

        return raw.copy(
            latitude = raw.latitude + offsetLatDeg,
            longitude = raw.longitude + offsetLonDeg,
            altitudeMeters = raw.altitudeMeters + offsetAlt
        )
    }

    /**
     * Raw dead-reckoning from report position using heading + speed + vertical rate.
     * No blend offset applied.
     */
    private fun predictRaw(track: TrackState, nowMs: Long): Position {
        val dtMs = (nowMs - track.reportTimeMs).coerceIn(0L, (MAX_EXTRAPOLATION_S * 1000).toLong())
        val dtSec = dtMs / 1000.0

        val heading = track.headingDeg ?: return track.reportPosition
        val speed = track.speedMps ?: return track.reportPosition

        if (speed < MIN_SPEED_MPS) return track.reportPosition

        // Apply turn rate to heading if significant
        val effectiveHeading = if (abs(track.turnRateDps) > MIN_TURN_RATE_DPS) {
            // Average heading over the extrapolation period (more accurate for turns)
            AngleUtils.normalizeAngle360(heading + track.turnRateDps * (dtSec / 2.0).toFloat())
        } else {
            heading
        }

        val distance = speed * dtSec
        val vertRate = track.verticalRateMps ?: 0f
        val newAlt = track.reportPosition.altitudeMeters + vertRate * dtSec

        // Spherical forward projection (destination point formula)
        val (newLat, newLon) = forwardProject(
            track.reportPosition.latitude,
            track.reportPosition.longitude,
            effectiveHeading,
            distance
        )

        return track.reportPosition.copy(
            latitude = newLat,
            longitude = newLon,
            altitudeMeters = newAlt
        )
    }

    /**
     * Spherical forward projection: given a start point, bearing, and distance,
     * compute the destination point on the Earth's surface.
     *
     * Uses the Vincenty direct formula (simplified for sphere):
     * lat2 = asin(sin(lat1)*cos(d/R) + cos(lat1)*sin(d/R)*cos(bearing))
     * lon2 = lon1 + atan2(sin(bearing)*sin(d/R)*cos(lat1), cos(d/R) - sin(lat1)*sin(lat2))
     */
    internal fun forwardProject(
        latDeg: Double, lonDeg: Double,
        bearingDeg: Float, distanceMeters: Double
    ): Pair<Double, Double> {
        val lat1 = Math.toRadians(latDeg)
        val lon1 = Math.toRadians(lonDeg)
        val bearing = Math.toRadians(bearingDeg.toDouble())
        val angularDist = distanceMeters / EARTH_RADIUS_M

        val lat2 = asin(
            sin(lat1) * cos(angularDist) +
                cos(lat1) * sin(angularDist) * cos(bearing)
        )
        val lon2 = lon1 + atan2(
            sin(bearing) * sin(angularDist) * cos(lat1),
            cos(angularDist) - sin(lat1) * sin(lat2)
        )

        return Pair(Math.toDegrees(lat2), Math.toDegrees(lon2))
    }

    /**
     * Clear all track state. Call when sensors stop or AR view pauses.
     */
    fun reset() {
        tracks.clear()
    }
}
