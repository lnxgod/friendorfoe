package com.friendorfoe.detection

/**
 * Selects one complete OpenDroneID payload from Android BLE service data.
 *
 * Android has already removed the FFFA service UUID. Broadcasters may expose
 * either a raw OpenDroneID payload or prepend the ASTM application code and a
 * rolling counter. Message Packs must remain intact for atomic parsing.
 */
internal object BleRemoteIdPayloadSelector {

    data class Selection(
        val payload: ByteArray,
        val transactionCounter: Int?
    )

    /**
     * Zero-copy description of the OpenDroneID bytes inside Android service data.
     * The scanner uses this on the BLE callback thread before deciding whether a
     * repeated radio frame needs to be copied and parsed on its worker.
     */
    data class Descriptor(
        val payloadOffset: Int,
        val payloadLength: Int,
        val transactionCounter: Int?,
        val messageType: Int,
    )

    private const val ASTM_ODID_APPLICATION_CODE = 0x0D
    private const val PREFIX_SIZE = 2
    private const val SINGLE_MESSAGE_SIZE = 25
    private const val MESSAGE_PACK_HEADER_SIZE = 3

    fun select(serviceData: ByteArray): Selection? {
        val descriptor = inspect(serviceData) ?: return null
        return Selection(
            payload = serviceData.copyOfRange(
                descriptor.payloadOffset,
                descriptor.payloadOffset + descriptor.payloadLength,
            ),
            transactionCounter = descriptor.transactionCounter,
        )
    }

    fun inspect(serviceData: ByteArray): Descriptor? {
        val payloadOffset: Int
        val transactionCounter: Int?
        if (isCompletePayload(serviceData, offset = 0, length = serviceData.size)) {
            payloadOffset = 0
            transactionCounter = null
        } else {
            if (serviceData.size <= PREFIX_SIZE ||
                (serviceData[0].toInt() and 0xFF) != ASTM_ODID_APPLICATION_CODE
            ) {
                return null
            }
            payloadOffset = PREFIX_SIZE
            transactionCounter = serviceData[1].toInt() and 0xFF
            if (!isCompletePayload(
                    serviceData,
                    offset = payloadOffset,
                    length = serviceData.size - payloadOffset,
                )
            ) {
                return null
            }
        }

        val messageType =
            (serviceData[payloadOffset].toInt() and 0xF0) ushr 4
        return Descriptor(
            payloadOffset = payloadOffset,
            payloadLength = serviceData.size - payloadOffset,
            transactionCounter = transactionCounter,
            messageType = messageType,
        )
    }

    private fun isCompletePayload(candidate: ByteArray, offset: Int, length: Int): Boolean {
        if (length <= 0 || offset < 0 || offset + length > candidate.size) return false
        val messageType = (candidate[offset].toInt() and 0xF0) ushr 4
        return if (messageType == OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK) {
            isCompleteMessagePack(candidate, offset, length)
        } else {
            length == SINGLE_MESSAGE_SIZE
        }
    }

    private fun isCompleteMessagePack(candidate: ByteArray, offset: Int, length: Int): Boolean {
        if (length < MESSAGE_PACK_HEADER_SIZE) return false
        val singleMessageSize = candidate[offset + 1].toInt() and 0xFF
        val messageCount = candidate[offset + 2].toInt() and 0xFF
        if (singleMessageSize != SINGLE_MESSAGE_SIZE || messageCount == 0) return false
        return length == MESSAGE_PACK_HEADER_SIZE + singleMessageSize * messageCount
    }
}
