package com.friendorfoe.detection

import kotlin.math.hypot
import kotlin.math.max

/**
 * Simple Online Realtime Tracker (SORT) for visual detections.
 *
 * Keeps AR labels alive between ML Kit detection frames by predicting bounding box
 * positions using velocity. When ML Kit misses a detection for a few frames
 * (common for distant aircraft), the tracker maintains the label using the last
 * known velocity.
 *
 * Features:
 * - Assigns stable tracking IDs across frames
 * - Predicts position using linear velocity model
 * - Keeps tracks alive for up to [MAX_COAST_FRAMES] without re-detection
 * - Kills tracks with inconsistent velocity (noise)
 * - Greedy nearest-neighbor assignment (not Hungarian — simpler, sufficient for <20 objects)
 */
class VisualTracker {

    companion object {
        /** Maximum frames to coast (predict without detection) before killing track */
        private const val MAX_COAST_FRAMES = 10

        /** Maximum normalized distance to associate a detection with a track */
        private const val MAX_MATCH_DISTANCE = 0.08f

        /** Minimum frames before a track is considered confirmed */
        private const val MIN_CONFIRM_FRAMES = 2
    }

    /** A tracked visual object with predicted position and velocity */
    data class Track(
        val trackId: Int,
        val centerX: Float,
        val centerY: Float,
        val width: Float,
        val height: Float,
        val velocityX: Float,
        val velocityY: Float,
        val age: Int,           // Total frames since track creation
        val hitCount: Int,      // How many times it was matched to a detection
        val coastFrames: Int,   // Consecutive frames without a match (0 = matched this frame)
        val confirmed: Boolean  // Whether this track has been confirmed (hitCount >= MIN_CONFIRM_FRAMES)
    )

    private var nextTrackId = 1
    private val activeTracks = mutableListOf<MutableTrack>()

    private class MutableTrack(
        val trackId: Int,
        var centerX: Float,
        var centerY: Float,
        var width: Float,
        var height: Float,
        var velocityX: Float = 0f,
        var velocityY: Float = 0f,
        var age: Int = 0,
        var hitCount: Int = 1,
        var coastFrames: Int = 0
    )

    /**
     * Update the tracker with a new set of detections.
     *
     * @param detections Current frame's visual detections (normalized 0-1 coordinates)
     * @return All active tracks (both matched and coasting)
     */
    fun update(detections: List<VisualDetection>): List<Track> {
        // Step 1: Predict existing tracks forward by their velocity
        for (track in activeTracks) {
            track.centerX += track.velocityX
            track.centerY += track.velocityY
            track.age++
        }

        // Step 2: Associate detections to tracks (greedy nearest-neighbor)
        val matched = BooleanArray(detections.size)
        val trackMatched = BooleanArray(activeTracks.size)

        // Sort by distance, match closest pairs first
        data class Pair(val trackIdx: Int, val detIdx: Int, val dist: Float)
        val pairs = mutableListOf<Pair>()

        for (ti in activeTracks.indices) {
            for (di in detections.indices) {
                val dx = activeTracks[ti].centerX - detections[di].centerX
                val dy = activeTracks[ti].centerY - detections[di].centerY
                val dist = hypot(dx, dy)
                if (dist < MAX_MATCH_DISTANCE) {
                    pairs.add(Pair(ti, di, dist))
                }
            }
        }

        pairs.sortBy { it.dist }

        for (pair in pairs) {
            if (trackMatched[pair.trackIdx] || matched[pair.detIdx]) continue
            trackMatched[pair.trackIdx] = true
            matched[pair.detIdx] = true

            val track = activeTracks[pair.trackIdx]
            val det = detections[pair.detIdx]

            // Update velocity (exponential moving average)
            val newVx = det.centerX - (track.centerX - track.velocityX) // Undo prediction, get true delta
            val newVy = det.centerY - (track.centerY - track.velocityY)
            track.velocityX = track.velocityX * 0.6f + newVx * 0.4f
            track.velocityY = track.velocityY * 0.6f + newVy * 0.4f

            // Snap position to detection
            track.centerX = det.centerX
            track.centerY = det.centerY
            track.width = det.width
            track.height = det.height
            track.hitCount++
            track.coastFrames = 0
        }

        // Step 3: Increment coast counter for unmatched tracks
        for (ti in activeTracks.indices) {
            if (!trackMatched[ti]) {
                activeTracks[ti].coastFrames++
            }
        }

        // Step 4: Remove dead tracks
        activeTracks.removeAll { it.coastFrames > MAX_COAST_FRAMES }

        // Step 5: Create new tracks for unmatched detections
        for (di in detections.indices) {
            if (!matched[di]) {
                val det = detections[di]
                activeTracks.add(MutableTrack(
                    trackId = nextTrackId++,
                    centerX = det.centerX,
                    centerY = det.centerY,
                    width = det.width,
                    height = det.height
                ))
            }
        }

        // Return all active tracks
        return activeTracks.map { track ->
            Track(
                trackId = track.trackId,
                centerX = track.centerX,
                centerY = track.centerY,
                width = track.width,
                height = track.height,
                velocityX = track.velocityX,
                velocityY = track.velocityY,
                age = track.age,
                hitCount = track.hitCount,
                coastFrames = track.coastFrames,
                confirmed = track.hitCount >= MIN_CONFIRM_FRAMES
            )
        }
    }

    /** Get the number of confirmed, active tracks */
    fun confirmedTrackCount(): Int = activeTracks.count {
        it.hitCount >= MIN_CONFIRM_FRAMES && it.coastFrames <= MAX_COAST_FRAMES
    }

    fun reset() {
        activeTracks.clear()
        nextTrackId = 1
    }
}
