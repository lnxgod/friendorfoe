package com.friendorfoe.detection

import java.util.ArrayDeque

enum class BlePromptFamily {
    APPLE_CONTINUITY,
    GOOGLE_FAST_PAIR,
    MICROSOFT_SWIFT_PAIR,
}

enum class BleSerialEvidence {
    SERIAL_UUID,
    SPARSE_PROFILE,
    GENERIC_NAME,
    PERSISTENT,
    CLOSE,
    CONNECTABLE,
    UNTRUSTED,
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

    data class SerialSkimmer(
        override val entityKey: String,
        val targetMac: String,
        val serialServiceUuid: Int,
        val strongestRssi: Int,
        val confidence: Float,
        val evidence: Set<BleSerialEvidence>,
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

    private data class SerialTrack(
        val targetMac: String,
        val serialServiceUuid: Int,
        val firstSeenAtMs: Long,
        var lastSeenAtMs: Long,
        var observationCount: Int,
        var strongestRssi: Int,
        var maxServiceCount: Int,
        var hasGenericName: Boolean,
        var isTrusted: Boolean,
        var hasPkocIdentity: Boolean,
        var seenConnectable: Boolean,
        var alerted: Boolean = false,
    )

    private val promptWindow = ArrayDeque<PromptSample>()
    private val lastDedupeAtMs = mutableMapOf<String, Long>()
    private val firstSeenBySignature = mutableMapOf<PromptSignature, MutableMap<String, Long>>()
    private val firstSeenOrder = ArrayDeque<PromptIdentity>()
    private val serialTracks = mutableMapOf<String, SerialTrack>()
    private var lastPromptSignalAtMs: Long? = null

    fun observe(observation: BleThreatObservation): List<BleThreatSignal> {
        val promptSignal = observePrompt(observation)
        val serialSignal = observeSerial(
            observation = observation,
            consumeSignal = promptSignal == null,
        )
        return listOfNotNull(promptSignal, serialSignal)
    }

    fun reset() {
        clearPromptState()
        serialTracks.clear()
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

    private fun observeSerial(
        observation: BleThreatObservation,
        consumeSignal: Boolean,
    ): BleThreatSignal.SerialSkimmer? {
        pruneSerialTracks(observation.observedAtMs)
        val serialServiceUuid = observation.serviceUuids16.firstOrNull { it in SERIAL_SERVICE_UUIDS } ?: return null
        val existingTrack = serialTracks[observation.mac]
        if (existingTrack != null && observation.observedAtMs - existingTrack.lastSeenAtMs <= DEDUPE_MS) {
            mergeDeduplicatedSerialEvidence(existingTrack, observation)
            return null
        }
        val track = existingTrack ?: createSerialTrack(observation, serialServiceUuid)
        if (existingTrack != null) updateSerialTrack(track, observation)
        if (track.alerted) return null
        if (track.isTrusted || track.hasPkocIdentity) return null

        val evidence = serialEvidence(track)
        if (!REQUIRED_SERIAL_EVIDENCE.all(evidence::contains)) return null
        if (evidence.count { it in SUPPORTING_SERIAL_EVIDENCE } < MIN_SUPPORTING_SERIAL_EVIDENCE) return null
        if (!consumeSignal) return null

        track.alerted = true
        return BleThreatSignal.SerialSkimmer(
            entityKey = "ble:serial-skimmer:${track.targetMac}",
            targetMac = track.targetMac,
            serialServiceUuid = track.serialServiceUuid,
            strongestRssi = track.strongestRssi,
            confidence = evidence.size.toFloat() / BleSerialEvidence.entries.size,
            evidence = evidence,
        )
    }

    private fun createSerialTrack(observation: BleThreatObservation, serialServiceUuid: Int): SerialTrack {
        while (serialTracks.size >= MAX_SERIAL_TRACKS) {
            val oldestMac = serialTracks.minByOrNull { it.value.lastSeenAtMs }?.key ?: break
            serialTracks.remove(oldestMac)
        }
        return SerialTrack(
            targetMac = observation.mac,
            serialServiceUuid = serialServiceUuid,
            firstSeenAtMs = observation.observedAtMs,
            lastSeenAtMs = observation.observedAtMs,
            observationCount = 1,
            strongestRssi = observation.rssi,
            maxServiceCount = observation.serviceUuids16.size,
            hasGenericName = observation.localName.isGenericSerialName(),
            isTrusted = observation.trustedIdentity,
            hasPkocIdentity = observation.localName.isPkocIdentity(),
            seenConnectable = observation.connectable,
        ).also { serialTracks[observation.mac] = it }
    }

    private fun mergeDeduplicatedSerialEvidence(
        track: SerialTrack,
        observation: BleThreatObservation,
    ) {
        track.strongestRssi = maxOf(track.strongestRssi, observation.rssi)
        track.isTrusted = track.isTrusted || observation.trustedIdentity
        track.hasPkocIdentity = track.hasPkocIdentity || observation.localName.isPkocIdentity()
    }

    private fun updateSerialTrack(track: SerialTrack, observation: BleThreatObservation) {
        track.lastSeenAtMs = observation.observedAtMs
        track.observationCount += 1
        track.strongestRssi = maxOf(track.strongestRssi, observation.rssi)
        track.maxServiceCount = maxOf(track.maxServiceCount, observation.serviceUuids16.size)
        track.hasGenericName = track.hasGenericName || observation.localName.isGenericSerialName()
        track.isTrusted = track.isTrusted || observation.trustedIdentity
        track.hasPkocIdentity = track.hasPkocIdentity || observation.localName.isPkocIdentity()
        track.seenConnectable = track.seenConnectable || observation.connectable
    }

    private fun pruneSerialTracks(nowMs: Long) {
        serialTracks.entries.removeAll { nowMs - it.value.lastSeenAtMs > config.clearAfterMs }
    }

    private fun serialEvidence(track: SerialTrack): Set<BleSerialEvidence> = buildSet {
        add(BleSerialEvidence.SERIAL_UUID)
        if (track.maxServiceCount == 1) add(BleSerialEvidence.SPARSE_PROFILE)
        if (track.hasGenericName) add(BleSerialEvidence.GENERIC_NAME)
        if (track.observationCount >= SERIAL_PERSISTENCE_OBSERVATIONS &&
            track.lastSeenAtMs - track.firstSeenAtMs >= SERIAL_PERSISTENCE_MS
        ) {
            add(BleSerialEvidence.PERSISTENT)
        }
        if (track.strongestRssi >= CLOSE_RSSI_DBM) add(BleSerialEvidence.CLOSE)
        if (track.seenConnectable) add(BleSerialEvidence.CONNECTABLE)
        if (!track.isTrusted && !track.hasPkocIdentity) add(BleSerialEvidence.UNTRUSTED)
    }

    private fun String?.isGenericSerialName(): Boolean =
        this?.trim()?.uppercase() in GENERIC_SERIAL_NAMES

    private fun String?.isPkocIdentity(): Boolean =
        this?.trim()?.uppercase()?.startsWith("PKOC") == true

    private companion object {
        const val DEDUPE_MS = 250L
        const val MAX_PROMPT_OBSERVATIONS = 256
        const val MAX_SERIAL_TRACKS = 64
        const val PAIRING_SPAM_ENTITY_KEY = "ble:pairing-spam"
        const val SERIAL_PERSISTENCE_OBSERVATIONS = 3
        const val SERIAL_PERSISTENCE_MS = 5_000L
        const val CLOSE_RSSI_DBM = -70
        val SERIAL_SERVICE_UUIDS = setOf(0xFFE0, 0xFFF0)
        val GENERIC_SERIAL_NAMES = setOf("BT", "BLE", "UART", "SERIAL", "HC-05", "HC-06")
        val REQUIRED_SERIAL_EVIDENCE = setOf(
            BleSerialEvidence.SERIAL_UUID,
            BleSerialEvidence.SPARSE_PROFILE,
            BleSerialEvidence.PERSISTENT,
        )
        val SUPPORTING_SERIAL_EVIDENCE = setOf(
            BleSerialEvidence.GENERIC_NAME,
            BleSerialEvidence.CLOSE,
            BleSerialEvidence.CONNECTABLE,
            BleSerialEvidence.UNTRUSTED,
        )
        const val MIN_SUPPORTING_SERIAL_EVIDENCE = 2
    }
}
