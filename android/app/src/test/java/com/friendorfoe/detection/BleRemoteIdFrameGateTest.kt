package com.friendorfoe.detection

import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class BleRemoteIdFrameGateTest {

    @Test
    fun suppressesOnlyExactRecentRepeatFromSameAdvertiserCounterAndType() {
        val gate = BleRemoteIdFrameGate(duplicateWindowNanos = 250_000_000L)
        val original = serviceData(counter = 7, messageType = OpenDroneIdParser.MSG_TYPE_BASIC_ID)
        val descriptor = requireNotNull(BleRemoteIdPayloadSelector.inspect(original))

        assertNotNull(gate.admit("AA", original, descriptor, 1_000_000_000L))
        assertNull(gate.admit("AA", original, descriptor, 1_100_000_000L))

        val changedBytes = original.copyOf().apply { this[lastIndex] = 1 }
        assertNotNull(gate.admit("AA", changedBytes, descriptor, 1_150_000_000L))
        assertNull(gate.admit("AA", original, descriptor, 1_175_000_000L))
        assertNotNull(gate.admit("BB", original, descriptor, 1_160_000_000L))

        val nextCounter = serviceData(
            counter = 8,
            messageType = OpenDroneIdParser.MSG_TYPE_BASIC_ID,
        )
        assertNotNull(
            gate.admit(
                "AA",
                nextCounter,
                requireNotNull(BleRemoteIdPayloadSelector.inspect(nextCounter)),
                1_170_000_000L,
            )
        )
    }

    @Test
    fun admitsExactFrameAgainAfterWindow() {
        val gate = BleRemoteIdFrameGate(duplicateWindowNanos = 250_000_000L)
        val frame = serviceData(counter = 9, messageType = OpenDroneIdParser.MSG_TYPE_LOCATION)
        val descriptor = requireNotNull(BleRemoteIdPayloadSelector.inspect(frame))

        assertNotNull(gate.admit("AA", frame, descriptor, 1_000_000_000L))
        assertNotNull(gate.admit("AA", frame, descriptor, 1_250_000_001L))
    }

    @Test
    fun rollbackAllowsRedundantRadioCopyToRetryFullWorkerQueue() {
        val gate = BleRemoteIdFrameGate()
        val frame = serviceData(counter = 10, messageType = OpenDroneIdParser.MSG_TYPE_SYSTEM)
        val descriptor = requireNotNull(BleRemoteIdPayloadSelector.inspect(frame))
        val admission = requireNotNull(gate.admit("AA", frame, descriptor, 1_000_000_000L))

        gate.rollback(admission)

        assertNotNull(gate.admit("AA", frame, descriptor, 1_025_000_000L))
    }

    private fun serviceData(counter: Int, messageType: Int): ByteArray =
        byteArrayOf(0x0D, counter.toByte()) + ByteArray(25).apply {
            this[0] = (messageType shl 4).toByte()
        }
}
