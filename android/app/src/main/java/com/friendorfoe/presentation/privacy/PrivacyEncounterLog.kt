package com.friendorfoe.presentation.privacy

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

private const val ENCOUNTER_RETENTION_MS = 30 * 60_000L
private const val MAX_ENCOUNTERS = 200
private const val MAX_SIGNAL_SAMPLES = 60

data class PrivacySignalSample(val elapsedMs: Long, val wallMs: Long, val dbm: Int)

data class PrivacyEncounter(
    val key: PrivacyFindingKey,
    val finding: PrivacyFinding,
    val firstObservedWallMs: Long,
    val lastObservedWallMs: Long,
    val lastObservedElapsedMs: Long,
    val observationCount: Int,
    val signalSamples: List<PrivacySignalSample>,
    val firstObservedElapsedMs: Long = lastObservedElapsedMs,
)

/** Session-local evidence: bounded, exact-source identities, no location or disk storage. */
internal class PrivacyEncounterLog {
    private val retained = linkedMapOf<PrivacyFindingKey, PrivacyEncounter>()
    private val _entries = MutableStateFlow<List<PrivacyEncounter>>(emptyList())
    val entries = _entries.asStateFlow()
    private var clearedThroughElapsedMs = Long.MIN_VALUE

    @Synchronized
    fun update(state: PrivacyCurrentState, ignoredKeys: Set<String>, nowElapsedMs: Long, nowWallMs: Long) {
        retained.entries.removeAll { (_, encounter) ->
            nowElapsedMs - encounter.lastObservedElapsedMs >= ENCOUNTER_RETENTION_MS ||
                encounter.finding.ignoreKey?.encoded in ignoredKeys
        }
        state.findings.forEach { finding ->
            val key = finding.routableKey ?: return@forEach
            if (finding.freshness != FindingFreshness.LIVE ||
                finding.lastObservedElapsedMs <= clearedThroughElapsedMs ||
                finding.lastObservedElapsedMs > nowElapsedMs ||
                nowElapsedMs - finding.lastObservedElapsedMs >= ENCOUNTER_RETENTION_MS ||
                finding.ignoreKey?.encoded in ignoredKeys
            ) return@forEach
            val previous = retained[key]
            if (previous != null && finding.lastObservedElapsedMs < previous.lastObservedElapsedMs) return@forEach
            if (previous != null && finding.lastObservedElapsedMs == previous.lastObservedElapsedMs) {
                retained[key] = previous.copy(finding = finding)
                return@forEach
            }
            val observedWallMs = nowWallMs - (nowElapsedMs - finding.lastObservedElapsedMs)
            val sample = finding.signalDbm?.takeIf { it in -127..-1 }?.let {
                PrivacySignalSample(finding.lastObservedElapsedMs, observedWallMs, it)
            }
            retained[key] = PrivacyEncounter(
                key = key,
                finding = finding,
                firstObservedWallMs = previous?.firstObservedWallMs ?: observedWallMs,
                lastObservedWallMs = observedWallMs,
                lastObservedElapsedMs = finding.lastObservedElapsedMs,
                observationCount = (previous?.observationCount ?: 0) + 1,
                firstObservedElapsedMs = previous?.firstObservedElapsedMs ?: finding.lastObservedElapsedMs,
                signalSamples = (previous?.signalSamples.orEmpty() + listOfNotNull(sample)).takeLast(MAX_SIGNAL_SAMPLES),
            )
        }
        val sorted = retained.values.sortedWith(
            compareByDescending<PrivacyEncounter> { it.lastObservedElapsedMs }.thenBy { it.key.encoded },
        ).take(MAX_ENCOUNTERS)
        retained.keys.retainAll(sorted.map { it.key }.toSet())
        // Reviewing an encounter must not become another packet-driven jumping list.
        _entries.value = sorted.sortedWith(
            compareByDescending<PrivacyEncounter> { it.firstObservedElapsedMs }.thenBy { it.key.encoded },
        )
    }

    @Synchronized
    fun clear(nowElapsedMs: Long) {
        clearedThroughElapsedMs = nowElapsedMs
        retained.clear()
        _entries.value = emptyList()
    }
}

internal fun signalTrend(samples: List<PrivacySignalSample>): String {
    if (samples.size < 6) return "Collecting signal samples"
    fun List<PrivacySignalSample>.median() = map { it.dbm }.sorted()[size / 2]
    val change = samples.takeLast(3).median() - samples.take(3).median()
    return when {
        change >= 6 -> "Signal strengthening"
        change <= -6 -> "Signal weakening"
        else -> "Signal broadly steady"
    }
}
