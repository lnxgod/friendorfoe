package com.friendorfoe.presentation.calibrate

import com.friendorfoe.data.remote.EventDto
import com.friendorfoe.data.remote.EventStatsDto
import com.friendorfoe.data.remote.NodeDto
import com.friendorfoe.data.remote.ProbeDeviceDto
import com.friendorfoe.data.remote.ScannerStatusDto

val PROBE_INTEL_EVENT_TYPES = listOf(
    "new_probe_identity",
    "new_probe_mac",
    "new_probed_ssid",
    "probe_activity_spike",
)

val PROBE_IDENTITY_EVENT_TYPES = setOf("new_probe_identity", "new_probe_mac")

fun probeIntelBadgeCount(stats: EventStatsDto): Int {
    return PROBE_INTEL_EVENT_TYPES.sumOf { stats.unackByType[it] ?: 0 }
}

fun sortNodesForDiagnostics(nodes: List<NodeDto>): List<NodeDto> {
    return nodes.sortedWith(
        compareByDescending<NodeDto> { nodeHealthScore(it) }
            .thenBy { it.name ?: it.deviceId }
    )
}

fun nodeHealthScore(node: NodeDto): Int {
    var score = 0
    if (!node.online) score += 1000
    score += maxScannerPressure(node.scanners) * 4
    if (node.scanners.any { (it.uartTxDropped ?: 0) > 0 || (it.probeDropPressure ?: 0) > 0 }) {
        score += 150
    }
    if (node.scanners.any { (it.noiseDropBle ?: 0) > 0 || (it.noiseDropWifi ?: 0) > 0 }) {
        score += 80
    }
    if (node.sourceFixupsRecent > 0) score += 50
    return score
}

fun maxScannerPressure(scanners: List<ScannerStatusDto>): Int {
    return scanners.maxOfOrNull { scanner ->
        scanner.txQueuePressurePct ?: queuePressureFromDepth(scanner)
    } ?: 0
}

fun activeNowProbes(probes: List<ProbeDeviceDto>, maxAgeS: Double = 300.0): List<ProbeDeviceDto> {
    return probes.filter { (it.ageS ?: Double.MAX_VALUE) <= maxAgeS }
        .sortedBy { it.ageS ?: Double.MAX_VALUE }
}

fun probeEventsForSection(events: List<EventDto>, eventTypes: Set<String>): List<EventDto> {
    return events.filter { it.eventType in eventTypes }
        .sortedByDescending { it.firstSeenAt }
}

private fun queuePressureFromDepth(scanner: ScannerStatusDto): Int {
    val depth = scanner.txQueueDepth ?: return 0
    val capacity = scanner.txQueueCapacity ?: return 0
    if (capacity <= 0) return 0
    return ((depth.toDouble() / capacity.toDouble()) * 100.0).toInt()
}
