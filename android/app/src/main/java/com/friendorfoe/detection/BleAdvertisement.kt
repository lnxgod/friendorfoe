package com.friendorfoe.detection

/**
 * Decoded BLE advertisement in a form downstream detectors can consume.
 *
 * Mirrors the BLE-specific subset of `esp32/shared/detection_types.h`
 * (`drone_detection_t`). Emitted by [BlePacketParser.parseAdvertisement]
 * from a raw `ScanResult`. Consumers: `GlassesDetector`, `BleFeatureExtractor`,
 * `PrivacyViewModel`.
 *
 * All fields are optional / nullable — a sparse advertisement still decodes.
 */
data class BleAdvertisement(
    val mac: String,
    val rssi: Int,

    // Raw structure
    val totalLength: Int,
    val adTypes: List<Int>,
    val payloadStructHash: Int,

    // Company ID
    val companyId: Int? = null,
    val rawMfr: ByteArray? = null,

    // AD type 0x01 (Flags)
    val advFlags: Int? = null,
    val dualModeHost: Boolean = false,   // (advFlags & ADV_FLAG_SIMUL_LE_BR_EDR_HOST) != 0

    // AD types 0x08 / 0x09 (Local Name)
    val localName: String? = null,

    // AD type 0x19 (Appearance)
    val appearance: Int? = null,

    // AD type 0x0A (TX Power)
    val txPower: Int? = null,

    // 16-bit Service UUIDs (AD types 0x02 / 0x03 / 0x16)
    val serviceUuids16: List<Int> = emptyList(),

    // Apple Continuity
    val apple: AppleContinuityDecoder.AppleContinuity? = null,

    // Microsoft Swift Pair
    val microsoft: MicrosoftSwiftPairDecoder.SwiftPair? = null,

    // BLE-JA3 32-bit structural hash (populated by BleFeatureExtractor)
    val ja3Hash: UInt? = null
) {
    /** Convenience: first 16-bit service UUID or null. */
    fun primaryServiceUuid16(): Int? = serviceUuids16.firstOrNull()

    /** True if advertisement declares a classic-BT host (phone/laptop). */
    fun isDualMode(): Boolean = dualModeHost
}
