package com.friendorfoe.detection

enum class PrivacyDetectionOrigin { ANDROID, BADGE, BACKEND, WIFI }

enum class BleInvestigationMode { GATT, PASSIVE_CAPTURE }

enum class BleInvestigationRoute { AUTO, PHONE, BADGE }

enum class BleInvestigationState {
    IDLE,
    QUEUED,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    READING,
    COMPLETE,
    FAILED,
    CANCELLED,
}

data class BleInvestigationTarget(
    val mode: BleInvestigationMode,
    val mac: String?,
    val entityKey: String,
    val observedAtElapsedMs: Long,
    val origin: PrivacyDetectionOrigin,
)

data class BleInvestigationRequest(
    val requestId: String,
    val target: BleInvestigationTarget,
    val route: BleInvestigationRoute,
    val timeoutMs: Long = 12_000,
)

data class BleGattCharacteristicInfo(
    val serviceUuid: String,
    val uuid: String,
    val properties: Set<String>,
)

data class BleInvestigationResult(
    val requestId: String,
    val transport: String,
    val mode: BleInvestigationMode,
    val targetMac: String?,
    val state: BleInvestigationState,
    val connectable: Boolean?,
    val services: List<String>,
    val characteristics: List<BleGattCharacteristicInfo>,
    val reads: Map<String, String>,
    val bonded: Boolean,
    val encrypted: Boolean,
    val authenticationRequired: Boolean,
    val summary: String,
    val error: String?,
    val truncated: Boolean,
)

sealed interface BleInvestigationChunk {
    val requestId: String

    data class Begin(
        override val requestId: String,
        val mode: BleInvestigationMode,
        val targetMac: String?,
    ) : BleInvestigationChunk

    data class Progress(
        override val requestId: String,
        val state: BleInvestigationState,
    ) : BleInvestigationChunk

    data class Service(
        override val requestId: String,
        val index: Int,
        val uuid: String,
    ) : BleInvestigationChunk

    data class Characteristic(
        override val requestId: String,
        val index: Int,
        val serviceUuid: String,
        val uuid: String,
        val properties: Set<String>,
    ) : BleInvestigationChunk

    data class Read(
        override val requestId: String,
        val index: Int,
        val uuid: String,
        val valueHex: String,
    ) : BleInvestigationChunk

    data class End(
        override val requestId: String,
        val state: String,
        val summary: String,
        val error: String? = null,
        val authenticationRequired: Boolean = false,
        val truncated: Boolean = false,
    ) : BleInvestigationChunk
}

class BleInvestigationChunkAssembler(private val requestId: String) {
    private var active = false
    private var mode = BleInvestigationMode.GATT
    private var targetMac: String? = null
    private var state = BleInvestigationState.IDLE
    private val services = mutableListOf<String>()
    private val characteristics = mutableListOf<BleGattCharacteristicInfo>()
    private val reads = linkedMapOf<String, String>()
    private var nextServiceIndex = 0
    private var nextCharacteristicIndex = 0
    private var nextReadIndex = 0
    private var truncated = false

    fun accept(chunk: BleInvestigationChunk): BleInvestigationResult? {
        if (chunk.requestId != requestId) return null

        return when (chunk) {
            is BleInvestigationChunk.Begin -> {
                reset(chunk)
                null
            }
            is BleInvestigationChunk.Progress -> {
                if (!active || !isForwardProgress(chunk.state)) return null
                state = chunk.state
                null
            }
            is BleInvestigationChunk.Service -> {
                if (!active || chunk.index != nextServiceIndex) return null
                nextServiceIndex++
                if (services.size < MAX_SERVICES) {
                    services += chunk.uuid
                } else {
                    truncated = true
                }
                null
            }
            is BleInvestigationChunk.Characteristic -> {
                if (!active || chunk.index != nextCharacteristicIndex) return null
                nextCharacteristicIndex++
                if (characteristics.size < MAX_CHARACTERISTICS) {
                    characteristics += BleGattCharacteristicInfo(
                        serviceUuid = chunk.serviceUuid,
                        uuid = chunk.uuid,
                        properties = chunk.properties.toSet(),
                    )
                } else {
                    truncated = true
                }
                null
            }
            is BleInvestigationChunk.Read -> {
                if (!active || chunk.index != nextReadIndex) return null
                nextReadIndex++
                if (nextReadIndex <= MAX_READS) {
                    reads[chunk.uuid] = chunk.valueHex.take(MAX_READ_HEX_CHARS)
                    if (chunk.valueHex.length > MAX_READ_HEX_CHARS) truncated = true
                } else {
                    truncated = true
                }
                null
            }
            is BleInvestigationChunk.End -> finish(chunk)
        }
    }

    private fun reset(chunk: BleInvestigationChunk.Begin) {
        active = true
        mode = chunk.mode
        targetMac = chunk.targetMac
        state = BleInvestigationState.QUEUED
        services.clear()
        characteristics.clear()
        reads.clear()
        nextServiceIndex = 0
        nextCharacteristicIndex = 0
        nextReadIndex = 0
        truncated = false
    }

    private fun isForwardProgress(next: BleInvestigationState): Boolean =
        next in PROGRESS_STATES && next.ordinal >= state.ordinal

    private fun finish(chunk: BleInvestigationChunk.End): BleInvestigationResult? {
        if (!active) return null
        val endState = BleInvestigationState.entries.firstOrNull {
            it.name.equals(chunk.state, ignoreCase = true)
        } ?: return null
        if (endState !in TERMINAL_STATES) return null

        state = endState
        active = false
        return BleInvestigationResult(
            requestId = requestId,
            transport = "badge",
            mode = mode,
            targetMac = targetMac,
            state = state,
            connectable = null,
            services = services.toList(),
            characteristics = characteristics.toList(),
            reads = reads.toMap(),
            bonded = false,
            encrypted = false,
            authenticationRequired = chunk.authenticationRequired,
            summary = chunk.summary,
            error = chunk.error,
            truncated = truncated || chunk.truncated,
        )
    }

    private companion object {
        const val MAX_SERVICES = 16
        const val MAX_CHARACTERISTICS = 32
        const val MAX_READS = 8
        const val MAX_READ_HEX_CHARS = 128

        val PROGRESS_STATES = setOf(
            BleInvestigationState.QUEUED,
            BleInvestigationState.SCANNING,
            BleInvestigationState.CONNECTING,
            BleInvestigationState.DISCOVERING,
            BleInvestigationState.READING,
        )
        val TERMINAL_STATES = setOf(
            BleInvestigationState.COMPLETE,
            BleInvestigationState.FAILED,
            BleInvestigationState.CANCELLED,
        )
    }
}

internal fun elapsedRealtimeMs(): Long = try {
    android.os.SystemClock.elapsedRealtime()
} catch (_: RuntimeException) {
    System.nanoTime() / 1_000_000L
}
