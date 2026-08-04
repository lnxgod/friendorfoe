package com.friendorfoe.detection

/**
 * Selects one complete OpenDroneID payload from Android BLE service data.
 *
 * Android has already removed the FFFA service UUID. Broadcasters may expose
 * either a raw OpenDroneID payload or prepend the ASTM application code and a
 * rolling counter. Message Packs must remain intact for atomic parsing.
 */
internal object BleRemoteIdPayloadSelector {

    private const val ASTM_ODID_APPLICATION_CODE = 0x0D
    private const val PREFIX_SIZE = 2
    private const val SINGLE_MESSAGE_SIZE = 25

    fun select(serviceData: ByteArray): ByteArray? {
        if (isCompletePayload(serviceData)) return serviceData.copyOf()

        if (serviceData.size <= PREFIX_SIZE ||
            (serviceData[0].toInt() and 0xFF) != ASTM_ODID_APPLICATION_CODE
        ) {
            return null
        }

        val candidate = serviceData.copyOfRange(PREFIX_SIZE, serviceData.size)
        return candidate.takeIf(::isCompletePayload)
    }

    private fun isCompletePayload(candidate: ByteArray): Boolean {
        if (candidate.isEmpty()) return false
        val messageType = (candidate[0].toInt() and 0xF0) ushr 4
        return if (messageType == OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK) {
            OpenDroneIdParser.isValidMessagePack(candidate)
        } else {
            candidate.size == SINGLE_MESSAGE_SIZE
        }
    }
}
