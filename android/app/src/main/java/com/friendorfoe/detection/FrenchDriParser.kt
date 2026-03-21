package com.friendorfoe.detection

import android.net.wifi.ScanResult
import android.os.Build
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Parser for French "Signalement Electronique a Distance" (Electronic Remote Identification)
 * per Arrete du 27 decembre 2019.
 *
 * French drones broadcast identification and telemetry in WiFi beacon vendor-specific
 * Information Elements using OUI 6A:5C:35, VS type 0x01. The payload uses TLV
 * (Type-Length-Value) encoding with the following fields:
 *
 * | Type | Field              | Encoding                          |
 * |------|--------------------|-----------------------------------|
 * | 1    | Protocol version   | uint8                             |
 * | 2    | ID_FR              | 30-char UTF-8 (trigram+model+SN)  |
 * | 3    | ANSI/CTA-2063 ID   | UTF-8 physical serial number      |
 * | 4    | Latitude           | ASCII decimal degrees [-90, +90]  |
 * | 5    | Longitude          | ASCII decimal degrees (-180, +180]|
 * | 6    | Altitude MSL       | ASCII meters                      |
 * | 7    | Height AGL         | ASCII meters                      |
 * | 8    | Takeoff latitude   | ASCII decimal degrees             |
 * | 9    | Takeoff longitude  | ASCII decimal degrees             |
 * | 10   | Ground speed       | ASCII m/s                         |
 * | 11   | Heading            | ASCII degrees 0-359               |
 *
 * Requires API 33+ for [ScanResult.getInformationElements].
 */
object FrenchDriParser {

    private const val TAG = "FrenchDriParser"

    /** French DRI OUI: 6A:5C:35 */
    private val FRENCH_OUI = byteArrayOf(0x6A, 0x5C, 0x35)

    /** VS type byte for French DRI protocol */
    private const val VS_TYPE: Byte = 0x01

    /** Vendor-specific IE element ID per IEEE 802.11 */
    private const val IE_VENDOR_SPECIFIC = 221

    /** Minimum IE payload: 3 (OUI) + 1 (type) + at least 1 TLV = 7 */
    private const val MIN_PAYLOAD_SIZE = 7

    // TLV type codes
    private const val TLV_PROTOCOL_VERSION = 1
    private const val TLV_ID_FR = 2
    private const val TLV_ANSI_CTA_ID = 3
    private const val TLV_LATITUDE = 4
    private const val TLV_LONGITUDE = 5
    private const val TLV_ALTITUDE_MSL = 6
    private const val TLV_HEIGHT_AGL = 7
    private const val TLV_TAKEOFF_LAT = 8
    private const val TLV_TAKEOFF_LON = 9
    private const val TLV_GROUND_SPEED = 10
    private const val TLV_HEADING = 11

    /**
     * Attempt to parse French DRI data from a WiFi scan result.
     *
     * @param scanResult WiFi scan result to inspect for French DRI vendor IEs
     * @param state Mutable accumulator for partial drone state (reused from ODID)
     * @return true if French DRI data was successfully parsed
     */
    fun parse(scanResult: ScanResult, state: OpenDroneIdParser.DronePartialState): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return false
        }

        return try {
            parseApi33(scanResult, state)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse French DRI IE: ${e.message}")
            false
        }
    }

    @Suppress("NewApi")
    private fun parseApi33(
        scanResult: ScanResult,
        state: OpenDroneIdParser.DronePartialState
    ): Boolean {
        val ies = scanResult.informationElements ?: return false

        for (ie in ies) {
            if (ie.id != IE_VENDOR_SPECIFIC) continue

            val bytes = ie.bytes ?: continue
            val payload = ByteArray(bytes.remaining())
            bytes.get(payload)
            bytes.rewind()

            if (payload.size < MIN_PAYLOAD_SIZE) continue

            // Check French OUI
            if (payload[0] != FRENCH_OUI[0] ||
                payload[1] != FRENCH_OUI[1] ||
                payload[2] != FRENCH_OUI[2]
            ) continue

            // Check VS type
            if (payload[3] != VS_TYPE) continue

            // Parse TLV fields starting at offset 4
            return parseTlvPayload(payload, 4, state)
        }

        return false
    }

    /**
     * Parse TLV-encoded fields from the French DRI payload.
     */
    private fun parseTlvPayload(
        payload: ByteArray,
        startOffset: Int,
        state: OpenDroneIdParser.DronePartialState
    ): Boolean {
        var offset = startOffset
        var parsed = false

        while (offset + 2 <= payload.size) {
            val tlvType = payload[offset].toInt() and 0xFF
            val tlvLength = payload[offset + 1].toInt() and 0xFF
            offset += 2

            if (offset + tlvLength > payload.size) break

            val valueBytes = payload.copyOfRange(offset, offset + tlvLength)
            val valueStr = String(valueBytes, Charsets.UTF_8).trim()

            when (tlvType) {
                TLV_PROTOCOL_VERSION -> {
                    // Protocol version, informational only
                }
                TLV_ID_FR -> {
                    if (valueStr.isNotEmpty()) {
                        state.droneId = valueStr
                        parsed = true
                    }
                }
                TLV_ANSI_CTA_ID -> {
                    // Physical serial number — use as droneId if ID_FR is absent
                    if (state.droneId == null && valueStr.isNotEmpty()) {
                        state.droneId = valueStr
                    }
                }
                TLV_LATITUDE -> {
                    valueStr.toDoubleOrNull()?.let { lat ->
                        if (lat in -90.0..90.0 && lat != 0.0) {
                            state.latitude = lat
                            parsed = true
                        }
                    }
                }
                TLV_LONGITUDE -> {
                    valueStr.toDoubleOrNull()?.let { lon ->
                        if (lon in -180.0..180.0 && lon != 0.0) {
                            state.longitude = lon
                            parsed = true
                        }
                    }
                }
                TLV_ALTITUDE_MSL -> {
                    valueStr.toDoubleOrNull()?.let { alt ->
                        state.altitudeMeters = alt
                    }
                }
                TLV_HEIGHT_AGL -> {
                    valueStr.toDoubleOrNull()?.let { height ->
                        state.heightAglMeters = height
                    }
                }
                TLV_TAKEOFF_LAT -> {
                    valueStr.toDoubleOrNull()?.let { lat ->
                        if (lat in -90.0..90.0 && lat != 0.0) {
                            state.operatorLatitude = lat
                        }
                    }
                }
                TLV_TAKEOFF_LON -> {
                    valueStr.toDoubleOrNull()?.let { lon ->
                        if (lon in -180.0..180.0 && lon != 0.0) {
                            state.operatorLongitude = lon
                        }
                    }
                }
                TLV_GROUND_SPEED -> {
                    valueStr.toFloatOrNull()?.let { speed ->
                        state.speedMps = speed
                    }
                }
                TLV_HEADING -> {
                    valueStr.toFloatOrNull()?.let { heading ->
                        if (heading in 0f..360f) {
                            state.heading = heading
                        }
                    }
                }
            }

            offset += tlvLength
        }

        if (parsed) {
            Log.d(TAG, "Parsed French DRI: id=${state.droneId}, " +
                    "lat=${state.latitude}, lon=${state.longitude}, " +
                    "alt=${state.altitudeMeters}m")
        }

        return parsed
    }
}
