package com.friendorfoe.detection

import android.util.Log
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import java.util.UUID

/**
 * Shared parser for OpenDroneID messages per ASTM F3411-22a.
 *
 * Used by both [RemoteIdScanner] (BLE transport) and [WifiNanRemoteIdScanner]
 * (WiFi NaN transport). The wire format of the 25-byte messages is identical
 * regardless of transport.
 */
object OpenDroneIdParser {

    private const val TAG = "OpenDroneIdParser"

    /** OpenDroneID BLE service UUID per ASTM F3411-19 / F3411-22a */
    val OPEN_DRONE_ID_UUID: UUID = UUID.fromString("0000FFFA-0000-1000-8000-00805F9B34FB")

    // OpenDroneID message types
    const val MSG_TYPE_BASIC_ID = 0
    const val MSG_TYPE_LOCATION = 1
    const val MSG_TYPE_AUTH = 2
    const val MSG_TYPE_SELF_ID = 3
    const val MSG_TYPE_SYSTEM = 4
    const val MSG_TYPE_OPERATOR_ID = 5
    const val MSG_TYPE_MESSAGE_PACK = 0xF

    // Location message field constants
    private const val LAT_LON_SCALE = 1e-7
    private const val ALT_SCALE = 0.5
    private const val ALT_OFFSET = -1000.0
    private const val SPEED_SCALE = 0.25f

    /**
     * Parse an OpenDroneID message and update the given partial state.
     *
     * @param messageData Raw 25-byte message (first nibble of byte 0 is message type)
     * @param state Mutable accumulator to update with parsed fields
     */
    fun parseMessage(messageData: ByteArray, state: DronePartialState, depth: Int = 0) {
        if (messageData.isEmpty()) return
        val messageType = (messageData[0].toInt() and 0xF0) ushr 4
        when (messageType) {
            MSG_TYPE_BASIC_ID -> parseBasicId(messageData, state)
            MSG_TYPE_LOCATION -> parseLocation(messageData, state)
            MSG_TYPE_SELF_ID -> parseSelfId(messageData, state)
            MSG_TYPE_SYSTEM -> parseSystem(messageData, state)
            MSG_TYPE_OPERATOR_ID -> parseOperatorId(messageData, state)
            MSG_TYPE_MESSAGE_PACK -> parseMessagePack(messageData, state, depth)
            else -> Log.d(TAG, "Unhandled OpenDroneID message type: $messageType")
        }
    }

    /**
     * Parse Basic ID message (Type 0).
     *
     * Format (25 bytes):
     *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
     *   Byte 1: [ID type (4 bits)] [UA type (4 bits)]
     *   Bytes 2-21: UAS ID (20 bytes, null-padded ASCII for serial number)
     *   Bytes 22-24: Reserved
     */
    private fun parseBasicId(data: ByteArray, state: DronePartialState) {
        if (data.size < 22) return

        val idTypeByte = data[1].toInt() and 0xFF
        val uaType = idTypeByte and 0x0F

        val serialBytes = data.copyOfRange(2, 22)
        val serial = String(serialBytes, Charsets.US_ASCII).trimEnd('\u0000').trim()

        if (serial.isNotEmpty()) {
            state.droneId = serial
        }
        state.uaType = uaType
        state.idType = (idTypeByte ushr 4) and 0x0F
    }

    /**
     * Parse Location/Vector message (Type 1).
     *
     * Format (25 bytes):
     *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
     *   Byte 1: Status flags
     *   Byte 2: Direction (heading / 2, so 0-179 maps to 0-358 degrees)
     *   Byte 3: Speed (horizontal, in 0.25 m/s increments)
     *   Byte 4: Vertical speed (int8, in 0.5 m/s increments)
     *   Bytes 5-8: Latitude (int32, x 1e-7 degrees)
     *   Bytes 9-12: Longitude (int32, x 1e-7 degrees)
     *   Bytes 13-14: Pressure altitude (uint16, x 0.5 - 1000 meters)
     *   Bytes 15-16: Geodetic altitude (uint16, x 0.5 - 1000 meters)
     *   Bytes 17-18: Height above ground (uint16, x 0.5 - 1000 meters)
     *   Byte 19: Horizontal/vertical accuracy
     *   Byte 20: Baro altitude accuracy / speed accuracy
     *   Bytes 21-22: Timestamp (uint16, tenths of seconds since the hour)
     *   Byte 23: Timestamp accuracy
     *   Byte 24: Reserved
     */
    private fun parseLocation(data: ByteArray, state: DronePartialState) {
        if (data.size < 17) return

        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

        val directionRaw = data[2].toInt() and 0xFF
        val heading = directionRaw * 2.0f

        val speedRaw = data[3].toInt() and 0xFF
        val speedMps = speedRaw * SPEED_SCALE

        val latRaw = buffer.getInt(5)
        val latitude = latRaw * LAT_LON_SCALE

        val lonRaw = buffer.getInt(9)
        val longitude = lonRaw * LAT_LON_SCALE

        val altRaw = buffer.getShort(13).toInt() and 0xFFFF
        val altitudeMeters = altRaw * ALT_SCALE + ALT_OFFSET

        if (latitude == 0.0 && longitude == 0.0) return
        if (latitude < -90.0 || latitude > 90.0) return
        if (longitude < -180.0 || longitude > 180.0) return

        state.latitude = latitude
        state.longitude = longitude
        state.altitudeMeters = altitudeMeters
        state.heading = if (heading <= 360f) heading else null
        state.speedMps = speedMps

        // Vertical speed (byte 4): int8, 0.5 m/s increments, 63 = unknown
        if (data.size >= 5) {
            val vsRaw = data[4].toInt() // signed byte
            if (vsRaw != 63) {
                state.verticalSpeedMps = vsRaw * 0.5f
            }
        }

        // Geodetic altitude (bytes 15-16): uint16, 0.5m - 1000m, 0xFFFF = unknown
        if (data.size >= 17) {
            val geoAltRaw = buffer.getShort(15).toInt() and 0xFFFF
            if (geoAltRaw != 0xFFFF) {
                state.geodeticAltitudeMeters = geoAltRaw * ALT_SCALE + ALT_OFFSET
            }
        }

        // Height AGL (bytes 17-18): uint16, 0.5m - 1000m, 0xFFFF = unknown
        if (data.size >= 19) {
            val heightRaw = buffer.getShort(17).toInt() and 0xFFFF
            if (heightRaw != 0xFFFF) {
                state.heightAglMeters = heightRaw * ALT_SCALE + ALT_OFFSET
            }
        }

        // Accuracy (byte 19): high nibble = horizontal, low nibble = vertical
        if (data.size >= 20) {
            val accByte = data[19].toInt() and 0xFF
            val hCode = (accByte ushr 4) and 0x0F
            val vCode = accByte and 0x0F
            state.horizontalAccuracyCode = hCode
            state.verticalAccuracyCode = vCode
        }

        // Timestamp (bytes 21-22): uint16, tenths of seconds since hour, 0xFFFF = unknown
        if (data.size >= 23) {
            val tsRaw = buffer.getShort(21).toInt() and 0xFFFF
            if (tsRaw != 0xFFFF) {
                state.locationTimestamp = tsRaw
            }
        }
    }

    /**
     * Parse System message (Type 4) - contains operator location.
     *
     * Format:
     *   Byte 1: Operator location type
     *   Bytes 2-5: Operator latitude (int32 * 1e-7)
     *   Bytes 6-9: Operator longitude (int32 * 1e-7)
     */
    private fun parseSystem(data: ByteArray, state: DronePartialState) {
        if (data.size < 10) return

        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

        val opLatRaw = buffer.getInt(2)
        val opLonRaw = buffer.getInt(6)

        val opLat = opLatRaw * LAT_LON_SCALE
        val opLon = opLonRaw * LAT_LON_SCALE

        if (opLat != 0.0 || opLon != 0.0) {
            if (opLat >= -90.0 && opLat <= 90.0 && opLon >= -180.0 && opLon <= 180.0) {
                state.operatorLatitude = opLat
                state.operatorLongitude = opLon
            }
        }

        // Area count (bytes 10-11)
        if (data.size >= 12) {
            state.areaCount = buffer.getShort(10).toInt() and 0xFFFF
        }
        // Area radius (bytes 12-13): x 10m
        if (data.size >= 14) {
            state.areaRadius = buffer.getShort(12).toInt() and 0xFFFF
        }
        // Area ceiling (bytes 14-15): x 0.5 - 1000
        if (data.size >= 16) {
            state.areaCeiling = (buffer.getShort(14).toInt() and 0xFFFF) * ALT_SCALE + ALT_OFFSET
        }
        // Area floor (bytes 16-17): x 0.5 - 1000
        if (data.size >= 18) {
            state.areaFloor = (buffer.getShort(16).toInt() and 0xFFFF) * ALT_SCALE + ALT_OFFSET
        }
        // Classification type (byte 18)
        if (data.size >= 19) {
            state.classificationTypeCode = data[18].toInt() and 0xFF
        }
    }

    /**
     * Parse Operator ID message (Type 5).
     *
     * Format:
     *   Byte 1: Operator ID type
     *   Bytes 2-21: Operator ID (20 bytes ASCII, null-padded)
     */
    private fun parseOperatorId(data: ByteArray, state: DronePartialState) {
        if (data.size < 22) return
        val operatorId = String(data, 2, 20, Charsets.US_ASCII).trimEnd('\u0000').trim()
        if (operatorId.isNotEmpty()) {
            state.operatorId = operatorId
        }
    }

    /**
     * Parse Self-ID message (Type 3).
     *
     * Format (25 bytes):
     *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
     *   Byte 1: Description type
     *   Bytes 2-24: 23-char ASCII text (null-padded)
     */
    private fun parseSelfId(data: ByteArray, state: DronePartialState) {
        if (data.size < 25) return
        state.selfIdDescriptionType = data[1].toInt() and 0xFF
        val text = String(data, 2, 23, Charsets.US_ASCII).trimEnd('\u0000').trim()
        if (text.isNotEmpty()) {
            state.selfIdText = text
        }
    }

    /**
     * Parse Message Pack (Type 0xF).
     *
     * Format:
     *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
     *   Byte 1: Message count
     *   Bytes 2+: N x 25-byte messages
     */
    private fun parseMessagePack(data: ByteArray, state: DronePartialState, depth: Int) {
        if (depth >= 2) {
            Log.w(TAG, "Message Pack recursion depth exceeded, skipping")
            return
        }
        if (data.size < 2) return
        val messageCount = data[1].toInt() and 0xFF
        val messagesStart = 2

        for (i in 0 until messageCount) {
            val offset = messagesStart + i * 25
            if (offset + 25 > data.size) break
            val msgData = data.copyOfRange(offset, offset + 25)
            parseMessage(msgData, state, depth + 1)
        }
    }

    /**
     * Convert ASTM F3411-22a accuracy code to meters.
     * Table 4: Horizontal/Vertical position accuracy.
     */
    fun accuracyCodeToMeters(code: Int): Float? = when (code) {
        0 -> null        // Unknown
        1 -> 18520f      // >= 18.52 km
        2 -> 7408f       // < 7.408 km
        3 -> 3704f       // < 3.704 km
        4 -> 1852f       // < 1.852 km (1 NM)
        5 -> 926f        // < 926 m
        6 -> 555.6f      // < 555.6 m
        7 -> 185.2f      // < 185.2 m
        8 -> 92.6f       // < 92.6 m
        9 -> 30f         // < 30 m
        10 -> 10f        // < 10 m
        11 -> 3f         // < 3 m
        12 -> 1f         // < 1 m
        else -> null
    }

    /**
     * Mutable accumulator for partial drone state.
     *
     * OpenDroneID messages may arrive in separate packets (BLE advertisements
     * or NaN service discovery events), so we accumulate data per device
     * identifier until we have enough to emit a Drone.
     */
    data class DronePartialState(
        val deviceAddress: String,
        val firstSeen: Instant,
        var lastUpdated: Instant = firstSeen,
        var droneId: String? = null,
        var uaType: Int? = null,
        var latitude: Double? = null,
        var longitude: Double? = null,
        var altitudeMeters: Double? = null,
        var heading: Float? = null,
        var speedMps: Float? = null,
        var operatorLatitude: Double? = null,
        var operatorLongitude: Double? = null,
        var operatorId: String? = null,
        var signalStrengthDbm: Int? = null,
        var rttDistanceMeters: Double? = null,
        var selfIdText: String? = null,
        var selfIdDescriptionType: Int? = null,
        var verticalSpeedMps: Float? = null,
        var geodeticAltitudeMeters: Double? = null,
        var heightAglMeters: Double? = null,
        var horizontalAccuracyCode: Int? = null,
        var verticalAccuracyCode: Int? = null,
        var locationTimestamp: Int? = null,
        var idType: Int? = null,
        var areaCount: Int? = null,
        var areaRadius: Int? = null,
        var areaCeiling: Double? = null,
        var areaFloor: Double? = null,
        var classificationTypeCode: Int? = null
    ) {
        /**
         * Convert to a Drone domain object if we have minimum required data.
         *
         * @param idPrefix Prefix for the drone ID ("rid_" for BLE, "nan_" for WiFi NaN)
         */
        fun toDroneOrNull(
            idPrefix: String = "rid_",
            detectionSource: DetectionSource = DetectionSource.REMOTE_ID
        ): Drone? {
            val id = droneId ?: deviceAddress

            val hasPosition = latitude != null && longitude != null
            val confidence = if (hasPosition) 0.9f else 0.6f

            return Drone(
                id = "$idPrefix$id",
                position = Position(
                    latitude = latitude ?: 0.0,
                    longitude = longitude ?: 0.0,
                    altitudeMeters = altitudeMeters ?: 0.0,
                    heading = heading,
                    speedMps = speedMps
                ),
                source = detectionSource,
                category = ObjectCategory.DRONE,
                confidence = confidence,
                firstSeen = firstSeen,
                lastUpdated = lastUpdated,
                droneId = id,
                operatorLatitude = operatorLatitude,
                operatorLongitude = operatorLongitude,
                signalStrengthDbm = signalStrengthDbm,
                estimatedDistanceMeters = rttDistanceMeters,
                operatorId = operatorId,
                uaType = uaType,
                selfIdText = selfIdText,
                verticalSpeedMps = verticalSpeedMps,
                heightAglMeters = heightAglMeters,
                geodeticAltitudeMeters = geodeticAltitudeMeters,
                horizontalAccuracyMeters = horizontalAccuracyCode?.let { accuracyCodeToMeters(it) },
                verticalAccuracyMeters = verticalAccuracyCode?.let { accuracyCodeToMeters(it) },
                idType = idType
            )
        }
    }
}
