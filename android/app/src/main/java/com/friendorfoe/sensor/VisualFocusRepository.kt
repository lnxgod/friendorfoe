package com.friendorfoe.sensor

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Shares recent camera-confirmed sky objects with non-AR screens.
 */
@Singleton
class VisualFocusRepository @Inject constructor() {

    companion object {
        const val DEFAULT_TTL_MS = 8_000L
    }

    data class VisualFocusEntry(
        val objectId: String,
        val lastSeenMs: Long,
        val screenX: Float,
        val screenY: Float
    )

    private val _entries = MutableStateFlow<Map<String, VisualFocusEntry>>(emptyMap())
    val entries: StateFlow<Map<String, VisualFocusEntry>> = _entries.asStateFlow()

    fun updateVisible(positions: List<ScreenPosition>, nowMs: Long = System.currentTimeMillis()) {
        val next = _entries.value
            .filterValues { nowMs - it.lastSeenMs <= DEFAULT_TTL_MS }
            .toMutableMap()

        positions
            .filter { it.isInView && it.visuallyConfirmed }
            .forEach { pos ->
                next[pos.skyObject.id] = VisualFocusEntry(
                    objectId = pos.skyObject.id,
                    lastSeenMs = nowMs,
                    screenX = pos.screenX,
                    screenY = pos.screenY
                )
            }

        _entries.value = next
    }

    fun activeEntries(nowMs: Long = System.currentTimeMillis()): Map<String, VisualFocusEntry> {
        return _entries.value.filterValues { nowMs - it.lastSeenMs <= DEFAULT_TTL_MS }
    }

    fun clear() {
        _entries.value = emptyMap()
    }
}
