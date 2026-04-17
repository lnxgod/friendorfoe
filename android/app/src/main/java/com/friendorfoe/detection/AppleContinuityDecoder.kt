package com.friendorfoe.detection

/**
 * Apple Continuity deep decoder.
 *
 * Direct Kotlin port of `classify_apple()` + the byte-offset extraction loop in
 * esp32/scanner/main/detection/ble_fingerprint.c (lines 149–205 and 264–296).
 * Extracts the furiousMAC-derived deep fields (auth tag, activity code,
 * data-flags byte, iOS version nibble, Nearby Action sub-type) in addition to
 * classifying the device.
 *
 * Input: the Apple manufacturer-specific data bytes — everything AFTER the
 * 2-byte company-ID prefix. If your source gives you bytes WITH the CID,
 * slice off the first 2 bytes before passing in.
 *
 * Preserves the v0.58 "honest Apple" stance: we do NOT guess iPhone vs iPad
 * vs Mac from Nearby Info / Nearby Action advertisements (Apple does not
 * broadcast the device model in those). Only the Tethering Source type
 * (0x0D) unambiguously indicates an iPhone.
 */
object AppleContinuityDecoder {

    enum class AppleDeviceType {
        UNKNOWN,
        /** Catch-all for Continuity messages that don't reveal the device model. */
        APPLE_GENERIC,
        /** Only safe when type byte is 0x0D (Tethering Source). */
        APPLE_IPHONE,
        APPLE_MACBOOK,
        APPLE_AIRPODS,
        APPLE_AIRTAG,
        /** Find My accessory that isn't an AirTag (Chipolo, Pebblebee, etc). */
        APPLE_FINDMY
    }

    /**
     * @property subType   raw mfr_data[2] byte (Continuity type).
     * @property deviceType see classification rules above.
     * @property authTag   bytes +3..+5 for types 0x0F / 0x10 (rotates slower than MAC).
     * @property activity  byte +6 for types 0x0F / 0x10 (0=idle, 1=audio, 2=phone, 3=video).
     * @property flagsByte byte +7 for types 0x0F / 0x10; decode bits via
     *                     [BleSignatures.APPLE_FLAG_*].
     * @property iosVersionNibble high nibble of byte +6 for type 0x10 — iOS major version.
     * @property nearbyActionSubType byte +3 for type 0x0F; see [BleSignatures.ACTION_*].
     */
    data class AppleContinuity(
        val subType: Int,
        val deviceType: AppleDeviceType,
        val authTag: ByteArray?,
        val activity: Int?,
        val flagsByte: Int?,
        val iosVersionNibble: Int?,
        val nearbyActionSubType: Int?
    ) {
        /** True if type 0x10 / 0x0F with any flag bits set. */
        fun hasFlagLabels(): Boolean = (flagsByte ?: 0) != 0

        /** Human-readable flag bullets (e.g. "AirPods in, Watch paired"), or null. */
        fun flagLabel(): String? {
            val bits = flagsByte ?: return null
            if (bits == 0) return null
            val labels = BleSignatures.nearbyFlagLabels(bits)
            return if (labels.isEmpty()) null else labels.joinToString(", ")
        }

        /** "Apple (AirPods in, Watch paired)" style label, or a plain "Apple Device". */
        fun enrichedLabel(): String {
            nearbyActionSubType?.let { sub ->
                BleSignatures.nearbyActionName(sub)?.let { return "Apple ($it)" }
            }
            flagLabel()?.let { return "Apple ($it)" }
            return when (deviceType) {
                AppleDeviceType.APPLE_IPHONE   -> "iPhone"
                AppleDeviceType.APPLE_MACBOOK  -> "Apple Mac/TV"
                AppleDeviceType.APPLE_AIRPODS  -> "AirPods"
                AppleDeviceType.APPLE_AIRTAG   -> "AirTag"
                AppleDeviceType.APPLE_FINDMY   -> "FindMy Accessory"
                AppleDeviceType.APPLE_GENERIC  -> "Apple Device"
                AppleDeviceType.UNKNOWN        -> "Apple Device"
            }
        }
    }

    /**
     * @param mfrData Apple manufacturer-specific data bytes AFTER the CID prefix.
     *                First byte should be the Continuity type byte (mfr_data[2]
     *                in ESP32 code using CID-inclusive indexing).
     */
    fun decode(mfrData: ByteArray): AppleContinuity? {
        if (mfrData.isEmpty()) return null

        val subType = mfrData[0].toInt() and 0xFF
        val deviceType = classifyAppleSubType(subType, mfrData)

        // Nearby Info (0x10) / Nearby Action (0x0F) share the same deep layout.
        // Match the ESP32 firmware byte offsets byte-for-byte — the firmware
        // uses CID-inclusive `ad_data[3..5]` for auth, `ad_data[6]` for
        // activity, `ad_data[7]` for flags. This Kotlin decoder receives
        // mfrData starting from the Continuity type byte (ad_data[2]), so
        // the Kotlin indices are: mfrData[1..3] auth, [4] activity, [5] flags.
        //
        // Reference: esp32/scanner/main/detection/ble_fingerprint.c lines 276–296.
        val authTag: ByteArray?
        val activity: Int?
        val flagsByte: Int?
        val iosVersionNibble: Int?
        val nearbyActionSubType: Int?

        if (subType == BleSignatures.APPLE_NEARBY_INFO || subType == BleSignatures.APPLE_NEARBY_ACTION) {
            authTag = if (mfrData.size >= 4) byteArrayOf(mfrData[1], mfrData[2], mfrData[3]) else null
            activity = if (mfrData.size >= 5) mfrData[4].toInt() and 0xFF else null
            flagsByte = if (mfrData.size >= 6) mfrData[5].toInt() and 0xFF else null
            iosVersionNibble = if (subType == BleSignatures.APPLE_NEARBY_INFO && mfrData.size >= 5) {
                (mfrData[4].toInt() and 0xF0) ushr 4
            } else null
            // Nearby Action (0x0F) puts the action sub-type in the sub-length
            // slot at mfrData[1]. We re-read it here as a typed helper.
            nearbyActionSubType = if (subType == BleSignatures.APPLE_NEARBY_ACTION && mfrData.size >= 2) {
                mfrData[1].toInt() and 0xFF
            } else null
        } else {
            authTag = null
            activity = null
            flagsByte = null
            iosVersionNibble = null
            nearbyActionSubType = null
        }

        return AppleContinuity(
            subType = subType,
            deviceType = deviceType,
            authTag = authTag,
            activity = activity,
            flagsByte = flagsByte,
            iosVersionNibble = iosVersionNibble,
            nearbyActionSubType = nearbyActionSubType
        )
    }

    /** Mirrors classify_apple() in ble_fingerprint.c. */
    private fun classifyAppleSubType(subType: Int, mfrData: ByteArray): AppleDeviceType {
        return when (subType) {
            BleSignatures.APPLE_FINDMY -> {
                // AirTags advertise 0x12 + length byte 0x19 (25 bytes of opaque payload).
                // Other FindMy accessories use different lengths.
                val lengthByte = if (mfrData.size >= 2) mfrData[1].toInt() and 0xFF else 0
                if (lengthByte == BleSignatures.APPLE_AIRTAG_LEN_BYTE) AppleDeviceType.APPLE_AIRTAG
                // Legacy fallback from firmware — very short FindMy payload is almost
                // always an AirTag (v0.47 behavior).
                else if (mfrData.size <= 8) AppleDeviceType.APPLE_AIRTAG
                else AppleDeviceType.APPLE_FINDMY
            }
            BleSignatures.APPLE_AIRPODS    -> AppleDeviceType.APPLE_AIRPODS
            BleSignatures.APPLE_HANDOFF    -> AppleDeviceType.APPLE_MACBOOK
            BleSignatures.APPLE_AIRPLAY    -> AppleDeviceType.APPLE_MACBOOK
            // Tethering Source is only broadcast by iPhones (no Mac or iPad acts as
            // a cellular hotspot source over Continuity). Safe to name.
            BleSignatures.APPLE_TETHER_SOURCE -> AppleDeviceType.APPLE_IPHONE
            // Tethering Target could be a Mac or iPad; stay honest.
            BleSignatures.APPLE_TETHER_TARGET -> AppleDeviceType.APPLE_GENERIC
            BleSignatures.APPLE_NEARBY_INFO   -> AppleDeviceType.APPLE_GENERIC
            BleSignatures.APPLE_NEARBY_ACTION -> AppleDeviceType.APPLE_GENERIC
            BleSignatures.APPLE_AIRDROP       -> AppleDeviceType.APPLE_GENERIC
            else -> AppleDeviceType.APPLE_GENERIC
        }
    }
}
