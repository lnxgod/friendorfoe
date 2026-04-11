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
