package com.friendorfoe.detection

import android.bluetooth.le.ScanResult
import android.util.Log
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Extracts ML-ready feature vectors from BLE advertisements.
 *
 * Features capture the structural "fingerprint" of a BLE device:
 * - Company ID presence/value
 * - Service UUIDs
 * - AD type sequence
 * - Payload structure (lengths, patterns)
 * - Advertisement interval estimation
 * - TX power and RSSI relationship
 *
 * These features are stable across MAC rotations and can be used
 * to train a classifier for unknown device types.
 */
@Singleton
class BleFeatureExtractor @Inject constructor() {

    companion object {
        private const val TAG = "BleFeatures"

        /**
         * Compute the 32-bit BLE-JA3 structural fingerprint hash.
         *
         * Byte-for-byte port of `ble_fingerprint_compute()` in
         * esp32/scanner/main/detection/ble_fingerprint.c (lines 222–377). Same
         * FNV-1a seeds, same field ordering, same per-AD-type handling. The
         * hash survives MAC rotation because it only mixes invariant
         * structure (address type, advertising props, AD-type sequence,
         * company ID, Apple sub-type, service UUIDs, payload length class).
         *
         * Output matches what a v0.58+ scanner emits for the same
         * advertisement so a future Android-to-backend forwarding PRD can
         * correlate phone observations with fleet observations directly.
         *
         * Static: called from the BLE scan callback hot path — no instantiation.
         *
         * @param addrType 0=public, 1=random.
         * @param props    advertising properties byte (legacy/ext, connectable,
         *                 scannable). Pass 0 if unavailable.
         */
        fun computeJa3Hash(result: ScanResult, addrType: Int, props: Int): UInt {
            val bytes = result.scanRecord?.bytes ?: return 0u
            return computeJa3HashBytes(bytes, addrType, props)
        }

        /** Pure-bytes variant of [computeJa3Hash] — exported for unit tests
         *  and for callers that don't have a `ScanResult`. */
        fun computeJa3HashBytes(bytes: ByteArray, addrType: Int, props: Int): UInt {
            var hash: UInt = BleSignatures.FNV1A_OFFSET_32.toUInt()

            fun mix(b: Int) {
                hash = (hash xor (b.toUInt() and 0xFFu))
                hash = hash * BleSignatures.FNV1A_PRIME_32.toUInt()
            }
            fun mixU16(v: Int) { mix(v and 0xFF); mix((v ushr 8) and 0xFF) }

            mix(addrType)
            mix(props)

            var pos = 0
            var totalLen = 0
            while (pos + 1 < bytes.size) {
                val adLen = bytes[pos].toInt() and 0xFF
                if (adLen == 0 || pos + 1 + adLen > bytes.size) break
                val adType = bytes[pos + 1].toInt() and 0xFF
                val adDataStart = pos + 2
                val adDataLen = adLen - 1

                mix(adType)
                totalLen += 1 + adLen

                when (adType) {
                    BleSignatures.AD_MANUFACTURER_SPECIFIC -> {
                        if (adDataLen >= 2) {
                            val cid = (bytes[adDataStart].toInt() and 0xFF) or
                                      ((bytes[adDataStart + 1].toInt() and 0xFF) shl 8)
                            mixU16(cid)
                            if (cid == BleSignatures.CID_APPLE && adDataLen >= 3) {
                                mix(bytes[adDataStart + 2].toInt() and 0xFF)
                                if (adDataLen >= 4) {
                                    mix(bytes[adDataStart + 3].toInt() and 0xFF)
                                }
                            }
                            if (cid == BleSignatures.CID_DJI && adDataLen >= 4) {
                                mix(bytes[adDataStart + 2].toInt() and 0xFF)
                                mix(bytes[adDataStart + 3].toInt() and 0xFF)
                            }
                        }
                    }
                    BleSignatures.AD_COMPLETE_UUID16,
                    BleSignatures.AD_INCOMPLETE_UUID16 -> {
                        var i = 0
                        while (i + 1 < adDataLen) {
                            val uuid = (bytes[adDataStart + i].toInt() and 0xFF) or
                                       ((bytes[adDataStart + i + 1].toInt() and 0xFF) shl 8)
                            mixU16(uuid)
                            i += 2
                        }
                    }
                    BleSignatures.AD_SERVICE_DATA_UUID16 -> {
                        if (adDataLen >= 2) {
                            val uuid = (bytes[adDataStart].toInt() and 0xFF) or
                                       ((bytes[adDataStart + 1].toInt() and 0xFF) shl 8)
                            mixU16(uuid)
                        }
                    }
                    BleSignatures.AD_FLAGS,
                    BleSignatures.AD_TX_POWER -> {
                        if (adDataLen >= 1) mix(bytes[adDataStart].toInt() and 0xFF)
                    }
                    else -> {
                        mix(adDataLen and 0xF0)
                    }
                }

                pos += 1 + adLen
            }

            mix((totalLen shr 2) shl 2)
            return hash
        }
    }

    data class BleFeatureVector(
        val hasManufacturerData: Boolean,
        val companyId: Int,              // 0 if none
        val manufacturerDataLen: Int,     // 0 if none
        val serviceUuid16Count: Int,      // Number of 16-bit service UUIDs
        val serviceUuid128Count: Int,     // Number of 128-bit service UUIDs
        val primaryUuid16: Int,           // First 16-bit UUID, 0 if none
        val hasDeviceName: Boolean,
        val deviceNameLen: Int,
        val totalAdLength: Int,           // Total advertisement payload length
        val adTypeCount: Int,             // Number of distinct AD types
        val adTypes: List<Int>,           // Ordered list of AD types present
        val txPower: Int,                 // TX power level, 127 if unknown
        val isConnectable: Boolean,
        val rssi: Int,
        val estimatedDistance: Float,     // From RSSI + TX power
        val addressType: Int,            // 0=public, 1=random
        // Byte structure fingerprint
        val payloadHash: Int,            // Hash of payload structure (not content)
        val hasAppleContinuity: Boolean,
        val appleSubType: Int,           // Apple continuity sub-type, 0 if not Apple
    ) {
        /** Convert to float array for ML model input */
        fun toFloatArray(): FloatArray = floatArrayOf(
            if (hasManufacturerData) 1f else 0f,
            companyId.toFloat(),
            manufacturerDataLen.toFloat(),
            serviceUuid16Count.toFloat(),
            serviceUuid128Count.toFloat(),
            primaryUuid16.toFloat(),
            if (hasDeviceName) 1f else 0f,
            deviceNameLen.toFloat(),
            totalAdLength.toFloat(),
            adTypeCount.toFloat(),
            txPower.toFloat(),
            if (isConnectable) 1f else 0f,
            rssi.toFloat(),
            estimatedDistance,
            addressType.toFloat(),
            payloadHash.toFloat(),
            if (hasAppleContinuity) 1f else 0f,
            appleSubType.toFloat()
        )
    }

    /**
     * Extract a feature vector from a BLE scan result.
     */
    fun extract(result: ScanResult): BleFeatureVector {
        val record = result.scanRecord
        val bytes = record?.bytes

        // Company ID and manufacturer data
        val mfrData = record?.manufacturerSpecificData
        val hasManufacturer = mfrData != null && mfrData.size() > 0
        val companyId = if (hasManufacturer) mfrData!!.keyAt(0) else 0
        val mfrDataLen = if (hasManufacturer) (mfrData!!.valueAt(0)?.size ?: 0) else 0

        // Service UUIDs
        val uuid16s = record?.serviceUuids?.filter { it.uuid.toString().length <= 8 } ?: emptyList()
        val uuid128s = record?.serviceUuids?.filter { it.uuid.toString().length > 8 } ?: emptyList()
        val primaryUuid16 = uuid16s.firstOrNull()?.uuid?.let {
            (it.mostSignificantBits shr 32).toInt() and 0xFFFF
        } ?: 0

        // Device name
        val name = record?.deviceName
        val hasName = !name.isNullOrEmpty()

        // AD types from raw bytes
        val adTypes = mutableListOf<Int>()
        if (bytes != null) {
            var pos = 0
            while (pos + 1 < bytes.size) {
                val len = bytes[pos].toInt() and 0xFF
                if (len == 0 || pos + 1 + len > bytes.size) break
                val type = bytes[pos + 1].toInt() and 0xFF
                adTypes.add(type)
                pos += 1 + len
            }
        }

        // TX Power
        val txPower = record?.txPowerLevel ?: 127

        // Apple Continuity detection
        var isApple = false
        var appleSubType = 0
        if (companyId == 0x004C && mfrData != null) {
            isApple = true
            val appleBytes = mfrData.get(0x004C)
            if (appleBytes != null && appleBytes.isNotEmpty()) {
                appleSubType = appleBytes[0].toInt() and 0xFF
            }
        }

        // Connectable
        val isConnectable = result.isConnectable

        // Address type (0=public, 1=random)
        val addressType = if (result.device.type == android.bluetooth.BluetoothDevice.DEVICE_TYPE_LE) 1 else 0

        // Payload structure hash (hash of lengths, not content)
        var structHash = 0
        if (bytes != null) {
            var pos = 0
            while (pos + 1 < bytes.size) {
                val len = bytes[pos].toInt() and 0xFF
                if (len == 0) break
                val type = bytes[pos + 1].toInt() and 0xFF
                structHash = structHash * 31 + (type shl 8 or len)
                pos += 1 + len
            }
        }

        // Distance estimation from RSSI + TX power
        val estimatedDistance = if (txPower != 127) {
            val ratio = (txPower - result.rssi).toFloat() / 20f
            Math.pow(10.0, ratio.toDouble()).toFloat()
        } else {
            -1f
        }

        return BleFeatureVector(
            hasManufacturerData = hasManufacturer,
            companyId = companyId,
            manufacturerDataLen = mfrDataLen,
            serviceUuid16Count = uuid16s.size,
            serviceUuid128Count = uuid128s.size,
            primaryUuid16 = primaryUuid16,
            hasDeviceName = hasName,
            deviceNameLen = name?.length ?: 0,
            totalAdLength = bytes?.size ?: 0,
            adTypeCount = adTypes.size,
            adTypes = adTypes,
            txPower = txPower,
            isConnectable = isConnectable,
            rssi = result.rssi,
            estimatedDistance = estimatedDistance,
            addressType = addressType,
            payloadHash = structHash,
            hasAppleContinuity = isApple,
            appleSubType = appleSubType
        )
    }
}
