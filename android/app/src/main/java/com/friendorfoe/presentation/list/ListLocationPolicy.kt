package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.Position

internal data class ListLocationFix(
    val position: Position,
    val elapsedRealtimeNanos: Long,
    val accuracyMeters: Float,
)

/** A recent network fix is preferable to stale GPS; delayed callbacks cannot rewind it. */
internal fun selectListLocationFix(
    candidates: List<ListLocationFix>,
    nowElapsedRealtimeNanos: Long,
): ListLocationFix? = candidates.filter { fix ->
    fix.position.latitude.isFinite() && fix.position.latitude in -90.0..90.0 &&
        fix.position.longitude.isFinite() && fix.position.longitude in -180.0..180.0 &&
        nowElapsedRealtimeNanos - fix.elapsedRealtimeNanos in 0..30_000_000_000L
}.maxWithOrNull(
    compareBy<ListLocationFix> { it.elapsedRealtimeNanos }
        .thenByDescending { it.accuracyMeters },
)
