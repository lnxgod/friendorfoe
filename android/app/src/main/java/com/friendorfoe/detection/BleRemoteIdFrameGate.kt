package com.friendorfoe.detection

import java.util.LinkedHashMap

/**
 * Suppresses only byte-identical BLE retransmissions from the same advertiser.
 *
 * ASTM BLE broadcasts intentionally repeat a payload across several radio
 * intervals. Keeping the first complete copy and moving it to the parser worker
 * preserves every distinct Basic ID, Location, System, Operator ID, and Message
 * Pack while avoiding repeated parsing on Android's main BLE callback thread.
 */
internal class BleRemoteIdFrameGate(
    private val duplicateWindowNanos: Long = DEFAULT_DUPLICATE_WINDOW_NANOS,
    private val maximumEntries: Int = DEFAULT_MAXIMUM_ENTRIES,
) {
    data class Admission internal constructor(
        internal val key: FrameKey,
        val serviceData: ByteArray,
        internal val observationTimestampNanos: Long,
    )

    internal data class FrameKey(
        val deviceAddress: String,
        val transactionCounter: Int?,
        val messageType: Int,
        val payloadHash: Int,
    )

    private data class RecentFrame(
        val serviceData: ByteArray,
        val observationTimestampNanos: Long,
    )

    private val recentFrames = object : LinkedHashMap<FrameKey, RecentFrame>(16, 0.75f, true) {
        override fun removeEldestEntry(
            eldest: MutableMap.MutableEntry<FrameKey, RecentFrame>?,
        ): Boolean = size > maximumEntries
    }

    init {
        require(duplicateWindowNanos >= 0L)
        require(maximumEntries > 0)
    }

    @Synchronized
    fun admit(
        deviceAddress: String,
        serviceData: ByteArray,
        descriptor: BleRemoteIdPayloadSelector.Descriptor,
        observationTimestampNanos: Long,
    ): Admission? {
        val key = FrameKey(
            deviceAddress = deviceAddress,
            transactionCounter = descriptor.transactionCounter,
            messageType = descriptor.messageType,
            payloadHash = serviceData.contentHashCode(),
        )
        val previous = recentFrames[key]
        if (previous != null) {
            val elapsed = observationTimestampNanos - previous.observationTimestampNanos
            if (elapsed in 0..duplicateWindowNanos &&
                previous.serviceData.contentEquals(serviceData)
            ) {
                return null
            }
        }

        val snapshot = serviceData.copyOf()
        recentFrames[key] = RecentFrame(snapshot, observationTimestampNanos)
        return Admission(key, snapshot, observationTimestampNanos)
    }

    /** Allow the next radio repeat through if the worker queue rejected this admission. */
    @Synchronized
    fun rollback(admission: Admission) {
        val current = recentFrames[admission.key] ?: return
        if (current.observationTimestampNanos == admission.observationTimestampNanos &&
            current.serviceData === admission.serviceData
        ) {
            recentFrames.remove(admission.key)
        }
    }

    @Synchronized
    fun clear() {
        recentFrames.clear()
    }

    private companion object {
        const val DEFAULT_DUPLICATE_WINDOW_NANOS = 250_000_000L
        const val DEFAULT_MAXIMUM_ENTRIES = 512
    }
}
