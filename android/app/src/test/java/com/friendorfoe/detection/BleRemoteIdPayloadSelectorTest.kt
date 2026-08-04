package com.friendorfoe.detection

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BleRemoteIdPayloadSelectorTest {

    @Test
    fun preservesRawSingleMessage() {
        val message = message(OpenDroneIdParser.MSG_TYPE_BASIC_ID)

        val selection = requireNotNull(BleRemoteIdPayloadSelector.select(message))
        assertArrayEquals(message, selection.payload)
        assertNull(selection.transactionCounter)
    }

    @Test
    fun stripsAppHeaderFromPrefixedSingleMessage() {
        val message = message(OpenDroneIdParser.MSG_TYPE_LOCATION)
        val serviceData = byteArrayOf(0x0D, 0x37) + message

        val selection = requireNotNull(BleRemoteIdPayloadSelector.select(serviceData))
        assertArrayEquals(message, selection.payload)
        assertEquals(0x37, selection.transactionCounter)
    }

    @Test
    fun preservesCompleteRawMessagePack() {
        val pack = fourMessagePack()

        assertEquals(103, pack.size)
        val selection = requireNotNull(BleRemoteIdPayloadSelector.select(pack))
        assertArrayEquals(pack, selection.payload)
        assertNull(selection.transactionCounter)
    }

    @Test
    fun stripsAppHeaderAndPreservesCompletePrefixedMessagePack() {
        val pack = fourMessagePack()
        val serviceData = byteArrayOf(0x0D, 0x7F) + pack

        assertEquals(105, serviceData.size)
        val selection = requireNotNull(BleRemoteIdPayloadSelector.select(serviceData))
        assertArrayEquals(pack, selection.payload)
        assertEquals(0x7F, selection.transactionCounter)
    }

    @Test
    fun rejectsMalformedAndTruncatedMessagePacks() {
        val wrongSize = fourMessagePack().apply { this[1] = 24 }
        val zeroCount = fourMessagePack().apply { this[2] = 0 }
        val truncated = fourMessagePack().copyOf(102)

        assertNull(BleRemoteIdPayloadSelector.select(wrongSize))
        assertNull(BleRemoteIdPayloadSelector.select(zeroCount))
        assertNull(BleRemoteIdPayloadSelector.select(truncated))
    }

    @Test
    fun rejectsUnknownPrefixAndShortPayloads() {
        val message = message(OpenDroneIdParser.MSG_TYPE_BASIC_ID)

        assertNull(BleRemoteIdPayloadSelector.select(byteArrayOf(0x01, 0x02) + message))
        assertNull(BleRemoteIdPayloadSelector.select(ByteArray(24)))
    }

    private fun fourMessagePack(): ByteArray {
        val messages = listOf(
            message(OpenDroneIdParser.MSG_TYPE_BASIC_ID),
            message(OpenDroneIdParser.MSG_TYPE_LOCATION),
            message(OpenDroneIdParser.MSG_TYPE_SYSTEM),
            message(OpenDroneIdParser.MSG_TYPE_OPERATOR_ID)
        )
        return ByteArray(103).also { pack ->
            pack[0] = (OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK shl 4).toByte()
            pack[1] = 25
            pack[2] = messages.size.toByte()
            messages.forEachIndexed { index, message ->
                message.copyInto(pack, destinationOffset = 3 + index * 25)
            }
        }
    }

    private fun message(type: Int): ByteArray = ByteArray(25).apply {
        this[0] = (type shl 4).toByte()
    }
}
