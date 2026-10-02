package com.friendorfoe.detection

/** FEAA is shared by Eddystone and Find Hub. The UUID alone identifies neither. */
object FeaaServiceClassifier {
    data class Match(
        val deviceType: String,
        val manufacturer: String,
        val confidence: Float,
        val reason: String,
        val evidence: String,
        val limitation: String,
    )

    // Google Find Hub accessory specification, advertised frames (160/256-bit EID,
    // with or without the optional hashed flags byte). Never decode encrypted flags
    // or correlate rotating identifiers as a physical-device identity.
    // https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn
    fun classify(data: ByteArray): Match? {
        val frame = data.firstOrNull()?.toInt()?.and(0xFF) ?: return null
        if (frame == 0x40 || frame == 0x41) {
            if (data.size != 21 && data.size != 22 && data.size != 33 && data.size != 34) return null
            return Match(
                deviceType = "Find Hub accessory",
                manufacturer = "Find Hub network",
                confidence = 0.95f,
                reason = "find_hub:0x${frame.toString(16)}",
                evidence = "Google Find Hub advertisement" + if (frame == 0x41) {
                    " · unwanted tracking protection mode advertised"
                } else {
                    " · normal advertising mode"
                },
                limitation = "This broadcast does not establish ownership or following. " +
                    "Tags and headphones can use Find Hub; rotating addresses may appear separately.",
            )
        }
        val eddystone = when (frame) {
            0x00 -> BlePacketParser.parseEddystoneUidServiceData(data) != null
            0x10 -> BlePacketParser.parseEddystoneUrlServiceData(data) != null
            0x20 -> BlePacketParser.parseEddystoneTlmServiceData(data) != null
            0x30 -> BlePacketParser.parseEddystoneEidServiceData(data) != null
            else -> false
        }
        return if (eddystone) Match(
            deviceType = "Eddystone Beacon",
            manufacturer = "Eddystone protocol",
            confidence = 0.70f,
            reason = "eddystone:0x${frame.toString(16)}",
            evidence = "Eddystone venue/location advertisement",
            limitation = "A beacon broadcast alone is not evidence of tracking or recording.",
        ) else null
    }
}
