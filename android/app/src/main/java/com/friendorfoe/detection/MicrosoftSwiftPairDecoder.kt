package com.friendorfoe.detection

/**
 * Microsoft Swift Pair decoder — Microsoft's equivalent of Apple Continuity for
 * peripheral pairing advertisements.
 *
 * Swift Pair wire format:
 *   mfr_data[0..1] = 0x06 0x00   (Microsoft CID, little-endian)
 *   mfr_data[2]    = 0x03        (Advertising Beacon sub-type)
 *   mfr_data[3]    = scenario byte (see table in [BleSignatures.microsoftSwiftPairLabel])
 *
 * Input: the Microsoft manufacturer-specific data bytes AFTER the CID prefix.
 * First byte should be the 0x03 beacon type.
 */
object MicrosoftSwiftPairDecoder {

    data class SwiftPair(
        val beaconType: Int,
        val scenario: Int,
        val label: String?
    )

    fun decode(mfrData: ByteArray): SwiftPair? {
        if (mfrData.size < 2) return null
        val beaconType = mfrData[0].toInt() and 0xFF
        if (beaconType != 0x03) return null  // Not the Advertising Beacon format
        val scenario = mfrData[1].toInt() and 0xFF
        return SwiftPair(
            beaconType = beaconType,
            scenario = scenario,
            label = BleSignatures.microsoftSwiftPairLabel(scenario)
        )
    }
}
