package com.friendorfoe.sensor

import com.friendorfoe.detection.ClassifiedVisualDetection
import com.friendorfoe.detection.VisualDetection

/**
 * Result of correlating radio-projected positions with visual detections.
 *
 * @property positions Radio positions with visual correlation applied (matched ones have [ScreenPosition.visuallyConfirmed] = true)
 * @property unmatchedVisuals Visual detections that did not match any radio position
 * @property classifiedUnknowns Unmatched visuals that have been classified and may warrant alerts
 */
data class CorrelationResult(
    val positions: List<ScreenPosition>,
    val unmatchedVisuals: List<VisualDetection>,
    val classifiedUnknowns: List<ClassifiedVisualDetection> = emptyList()
)
