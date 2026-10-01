package com.friendorfoe.presentation.probes

import com.friendorfoe.data.remote.ProbeActivityDto
import com.friendorfoe.data.remote.ProbeTransmitterDto

const val PROBE_WINDOW_SECONDS = 300

data class ProbeUiState(
    val enabled: Boolean = false,
    val usbSource: Boolean = false,
    val usb: com.friendorfoe.data.probes.UsbProbeState = com.friendorfoe.data.probes.UsbProbeState(),
    val loading: Boolean = false,
    val sensorId: String? = null,
    val snapshot: ProbeActivityDto = ProbeActivityDto(),
    val snapshotElapsedMs: Long? = null,
    val nowElapsedMs: Long = 0,
    val error: String? = null,
    val query: String = "",
    val minimumRssi: Int? = null,
    val directedOnly: Boolean = false,
    val strongestFirst: Boolean = false,
    val announceNew: Boolean = false,
    val newActivity: String? = null,
) {
    val elapsedSeconds: Double get() = snapshotElapsedMs?.let {
        (nowElapsedMs - it).coerceAtLeast(0) / 1000.0
    } ?: 0.0
    val stale: Boolean get() = error != null || snapshotElapsedMs == null || elapsedSeconds > 20
    val rows: List<ProbeTransmitterDto> get() {
        val needle = query.trim()
        val rows = snapshot.transmitters.filter {
            it.sensorId == sensorId && it.ageSeconds.isFinite() && it.ageSeconds >= 0 &&
                it.ageSeconds + elapsedSeconds <= PROBE_WINDOW_SECONDS &&
                (!directedOnly || it.targets.isNotEmpty()) &&
                (minimumRssi == null || (it.rssi?.takeIf { signal -> signal in -127..-1 } ?: -128) >= minimumRssi) &&
                (needle.isEmpty() || it.mac.contains(needle, true) || it.targets.any { target -> target.ssid.contains(needle, true) })
        }
        return if (strongestFirst) rows.sortedWith(compareByDescending<ProbeTransmitterDto> { it.rssi ?: -128 }
            .thenBy { it.ageSeconds }.thenBy { it.mac })
        else rows.sortedWith(compareBy<ProbeTransmitterDto> { it.ageSeconds }.thenByDescending { it.rssi ?: -128 }.thenBy { it.mac })
    }
}

fun probeAgeLabel(seconds: Double): String = when {
    !seconds.isFinite() || seconds < 0 -> "Time unavailable"
    seconds < 5 -> "Just heard"
    seconds < 60 -> "${seconds.toInt()}s ago"
    else -> "${(seconds / 60).toInt()}m ago"
}

/** Baseline the first successful poll; announce only new addresses or targets, not every poll. */
class ProbeActivityChanges {
    private var baseline = false
    private val seen = linkedSetOf<String>()
    private var lastSnapshot = emptySet<String>()
    fun reset() { baseline = false; seen.clear(); lastSnapshot = emptySet() }
    fun update(rows: List<ProbeTransmitterDto>): Int {
        val keysByRow = rows.map { row ->
            listOf("${row.sensorId}/${row.mac}") + row.targets.map { "${row.sensorId}/${row.mac}/ssid:${it.ssid}" } +
                if (row.wildcardReports > 0) listOf("${row.sensorId}/${row.mac}/wildcard") else emptyList()
        }
        val changed = keysByRow.count { keys -> keys.any { it !in seen && it !in lastSnapshot } }
        val keys = keysByRow.flatten()
        seen.addAll(keys)
        lastSnapshot = keys.toSet()
        while (seen.size > 4096) seen.remove(seen.first())
        val result = if (baseline) changed else 0
        baseline = true
        return result
    }
}
