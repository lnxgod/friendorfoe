package com.friendorfoe.detection

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GlassesDetectorRemoteIdPolicyTest {

    @Test
    fun `complete raw remote id message bypasses privacy classification`() {
        val basicId = remoteIdMessage(OpenDroneIdParser.MSG_TYPE_BASIC_ID)

        assertTrue(GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(basicId))
    }

    @Test
    fun `ASTM prefixed remote id message bypasses privacy classification`() {
        val serviceData = byteArrayOf(0x0D, 0x37) +
            remoteIdMessage(OpenDroneIdParser.MSG_TYPE_LOCATION)

        assertTrue(GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(serviceData))
    }

    @Test
    fun `complete message pack bypasses privacy classification`() {
        val messages = listOf(
            remoteIdMessage(OpenDroneIdParser.MSG_TYPE_BASIC_ID),
            remoteIdMessage(OpenDroneIdParser.MSG_TYPE_LOCATION),
        )
        val pack = ByteArray(3 + messages.size * REMOTE_ID_MESSAGE_SIZE).also { data ->
            data[0] = (OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK shl 4).toByte()
            data[1] = REMOTE_ID_MESSAGE_SIZE.toByte()
            data[2] = messages.size.toByte()
            messages.forEachIndexed { index, message ->
                message.copyInto(data, destinationOffset = 3 + index * REMOTE_ID_MESSAGE_SIZE)
            }
        }

        assertTrue(GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(pack))
    }

    @Test
    fun `missing or malformed remote id data remains eligible for privacy classification`() {
        assertFalse(GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(null))
        assertFalse(GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(ByteArray(24)))
        assertFalse(
            GlassesDetector.shouldIgnoreOpenDroneIdPrivacyFrame(
                byteArrayOf(0x0D, 0x37) + ByteArray(24),
            ),
        )
    }

    private fun remoteIdMessage(messageType: Int) = ByteArray(REMOTE_ID_MESSAGE_SIZE).also {
        it[0] = (messageType shl 4).toByte()
    }

    private companion object {
        const val REMOTE_ID_MESSAGE_SIZE = 25
    }
}
