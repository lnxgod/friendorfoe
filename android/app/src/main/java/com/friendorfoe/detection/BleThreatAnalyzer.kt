package com.friendorfoe.detection

import java.util.ArrayDeque

enum class BlePromptFamily {
    APPLE_CONTINUITY,
    GOOGLE_FAST_PAIR,
    MICROSOFT_SWIFT_PAIR,
}

data class BleThreatObservation(
    val mac: String,
    val observedAtMs: Long,
    val rssi: Int,
    val connectable: Boolean,
    val structuralHash: Int,
    val promptFamily: BlePromptFamily?,
    val serviceUuids16: Set<Int>,
    val localName: String?,
    val companyId: Int?,
    val trustedIdentity: Boolean,
)

sealed interface BleThreatSignal {
    val entityKey: String

    data class PairingSpam(
        override val entityKey: String,
        val families: Set<BlePromptFamily>,
        val uniqueMacs: Int,
        val observationCount: Int,
        val strongestRssi: Int,
        val rssiSpan: Int,
        val windowMs: Long,
    ) : BleThreatSignal
}

class BleThreatAnalyzer(
    private val config: Config = Config(),
) {
    data class Config(
        val windowMs: Long = 8_000,
        val minUniqueMacs: Int = 12,
        val minObservations: Int = 24,
        val minChurnRatio: Double = 0.75,
        val minRatePerSecond: Double = 3.0,
        val maxRssiSpan: Int = 20,
        val maxRssiIqr: Int = 12,
        val cooldownMs: Long = 60_000,
        val clearAfterMs: Long = 20_000,
    )

    internal data class DebugSnapshot(
        val deduplicatedObservations: Int,
        val firstSeenIdentities: Int,
        val signatureBuckets: Int,
        val dedupeEntries: Int,
    )

    private data class PromptSample(
        val observation: BleThreatObservation,
        val signature: PromptSignature,
    )

    private data class PromptSignature(
        val family: BlePromptFamily,
        val structuralHash: Int,
    )

    private data class PromptIdentity(
        val signature: PromptSignature,
        val mac: String,
    )

    private val promptWindow = ArrayDeque<PromptSample>()
    private val lastDedupeAtMs = mutableMapOf<String, Long>()
    private val firstSeenBySignature = mutableMapOf<PromptSignature, MutableMap<String, Long>>()
    private val firstSeenOrder = ArrayDeque<PromptIdentity>()
    private var lastPromptSignalAtMs: Long? = null

    fun observe(observation: BleThreatObservation): List<BleThreatSignal> =
        listOfNotNull(observePrompt(observation))

    fun reset() {
        clearPromptState()
    }

    private fun clearPromptState() {
        promptWindow.clear()
        lastDedupeAtMs.clear()
        firstSeenBySignature.clear()
        firstSeenOrder.clear()
        lastPromptSignalAtMs = null
    }

    internal fun debugSnapshot(): DebugSnapshot = DebugSnapshot(
        deduplicatedObservations = promptWindow.size,
        firstSeenIdentities = firstSeenOrder.size,
        signatureBuckets = firstSeenBySignature.size,
        dedupeEntries = lastDedupeAtMs.size,
    )

    private fun observePrompt(observation: BleThreatObservation): BleThreatSignal.PairingSpam? {
        val family = observation.promptFamily ?: return null
        prunePromptState(observation.observedAtMs)

        val dedupeKey = promptDedupeKey(observation, family)
        val lastSeenAtMs = lastDedupeAtMs[dedupeKey]
        if (lastSeenAtMs != null && observation.observedAtMs - lastSeenAtMs <= DEDUPE_MS) {
            return null
        }
        lastDedupeAtMs[dedupeKey] = observation.observedAtMs

        val signature = PromptSignature(family, observation.structuralHash)
        rememberFirstSeen(signature, observation)
        while (promptWindow.size >= MAX_PROMPT_OBSERVATIONS) {
            evictOldestPromptSample()
        }
        promptWindow.addLast(PromptSample(observation, signature))

        val signal = pairingSpamSignal(observation.observedAtMs) ?: return null
        lastPromptSignalAtMs = observation.observedAtMs
        return signal
    }

    private fun prunePromptState(nowMs: Long) {
        if (promptWindow.isNotEmpty() && nowMs - promptWindow.last.observation.observedAtMs >= config.clearAfterMs) {
            clearPromptState()
            return
        }
        while (promptWindow.isNotEmpty() && nowMs - promptWindow.first.observation.observedAtMs > config.windowMs) {
            promptWindow.removeFirst()
        }
        lastDedupeAtMs.entries.removeAll { nowMs - it.value > DEDUPE_MS }
    }

    private fun pairingSpamSignal(nowMs: Long): BleThreatSignal.PairingSpam? {
        val lastSignalAtMs = lastPromptSignalAtMs
        if (lastSignalAtMs != null && nowMs - lastSignalAtMs < config.cooldownMs) return null

        if (promptWindow.size < config.minObservations) return null
        val observations = promptWindow.map { it.observation }
        val uniqueMacs = observations.mapTo(mutableSetOf()) { it.mac }
        if (uniqueMacs.size < config.minUniqueMacs) return null
        val windowStartMs = promptWindow.first.observation.observedAtMs
        val churnedMacs = promptWindow
            .associateBy({ it.observation.mac }, { it.signature })
            .count { (mac, signature) ->
                (firstSeenBySignature[signature]?.get(mac) ?: Long.MIN_VALUE) >= windowStartMs
            }
        if (churnedMacs.toDouble() / uniqueMacs.size < config.minChurnRatio) return null

        val rssi = observations.map { it.rssi }.sorted()
        val rssiSpan = rssi.last() - rssi.first()
        if (rssiSpan > config.maxRssiSpan || rssiIqr(rssi) > config.maxRssiIqr) return null
        if (observations.size.toDouble() / (config.windowMs / 1_000.0) < config.minRatePerSecond) return null

        return BleThreatSignal.PairingSpam(
            entityKey = PAIRING_SPAM_ENTITY_KEY,
            families = observations.mapTo(mutableSetOf()) { it.promptFamily!! },
            uniqueMacs = uniqueMacs.size,
            observationCount = observations.size,
            strongestRssi = rssi.last(),
            rssiSpan = rssiSpan,
            windowMs = config.windowMs,
        )
    }

    private fun rssiIqr(sortedRssi: List<Int>): Int {
        val firstQuartile = sortedRssi[sortedRssi.size / 4]
        val thirdQuartile = sortedRssi[(sortedRssi.size * 3) / 4]
        return thirdQuartile - firstQuartile
    }

    private fun rememberFirstSeen(signature: PromptSignature, observation: BleThreatObservation) {
        val firstSeenForSignature = firstSeenBySignature.getOrPut(signature) { mutableMapOf() }
        if (firstSeenForSignature.putIfAbsent(observation.mac, observation.observedAtMs) != null) return

        while (firstSeenOrder.size >= MAX_PROMPT_OBSERVATIONS) {
            evictOldestFirstSeenIdentity()
        }
        firstSeenOrder.addLast(PromptIdentity(signature, observation.mac))
    }

    private fun evictOldestPromptSample() {
        val oldest = promptWindow.removeFirst()
        val dedupeKey = promptDedupeKey(oldest.observation, oldest.signature.family)
        if (lastDedupeAtMs[dedupeKey] == oldest.observation.observedAtMs) {
            lastDedupeAtMs.remove(dedupeKey)
        }
    }

    private fun evictOldestFirstSeenIdentity() {
        val oldest = firstSeenOrder.removeFirst()
        val firstSeenForSignature = firstSeenBySignature[oldest.signature] ?: return
        firstSeenForSignature.remove(oldest.mac)
        if (firstSeenForSignature.isEmpty()) {
            firstSeenBySignature.remove(oldest.signature)
        }
    }

    private fun promptDedupeKey(observation: BleThreatObservation, family: BlePromptFamily): String =
        "${observation.mac}|${observation.structuralHash}|$family"

    private companion object {
        const val DEDUPE_MS = 250L
        const val MAX_PROMPT_OBSERVATIONS = 256
        const val PAIRING_SPAM_ENTITY_KEY = "ble:pairing-spam"
    }
}
