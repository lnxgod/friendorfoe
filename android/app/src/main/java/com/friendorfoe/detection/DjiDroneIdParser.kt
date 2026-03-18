package com.friendorfoe.detection

import android.net.wifi.ScanResult
import android.os.Build
import android.util.Log

/**
 * Parser for DJI DroneID vendor-specific Information Elements (IEs) found in
 * WiFi beacon frames.
 *
 * DJI drones embed flight telemetry in vendor-specific IEs using OUI 26:37:12.
 * The IE payload contains GPS coordinates, altitude, speed, serial number, and
 * the operator's home point. This provides full position data from a WiFi scan
 * — no Remote ID broadcast required.
 *
 * IE format (after 3-byte OUI):
 *   Byte 0:    Version/type indicator
 *   Bytes 1-4: Serial number (first 4 chars, ASCII) — full serial in later block
 *   Bytes 5-8: Drone longitude (int32, degrees × 1e7)
 *   Bytes 9-12: Drone latitude (int32, degrees × 1e7)
 *   Bytes 13-14: Altitude (int16, meters relative to takeoff point)
 *   Bytes 15-16: Height above ground (int16, decimeters)
 *   Bytes 17-18: Speed (uint16, cm/s)
 *   Bytes 19-20: Heading (int16, degrees × 100, 0 = North)
 *   Bytes 21-24: Home longitude (int32, degrees × 1e7)
 *   Bytes 25-28: Home latitude (int32, degrees × 1e7)
 *
 * Reference: Department of Defense CUAS DroneID analysis, open-source
 * implementations (e.g., kismet, opendroneid-dissector).
 *
 * Requires API 33+ for [ScanResult.getInformationElements].
 */
object DjiDroneIdParser {

    private const val TAG = "DjiDroneIdParser"

    /** DJI's vendor-specific OUI for DroneID IEs */
    private val DJI_OUI = byteArrayOf(0x26, 0x37, 0x12)

    /** Vendor-specific IE element ID per IEEE 802.11 */
    private const val IE_VENDOR_SPECIFIC = 221

    /** Minimum payload length after OUI to contain position data */
    private const val MIN_PAYLOAD_LENGTH = 29

    /**
     * Attempt to parse DJI DroneID data from a WiFi scan result.
     *
     * @param scanResult A WiFi scan result to inspect for vendor IEs
     * @return Parsed [DjiDroneIdData] if a valid DroneID IE is found, null otherwise
     */
    fun parse(scanResult: ScanResult): DjiDroneIdData? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return null // getInformationElements() requires API 33
        }

        return try {
            parseApi33(scanResult)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse DroneID IE: ${e.message}")
            null
        }
    }

    @Suppress("NewApi")
    private fun parseApi33(scanResult: ScanResult): DjiDroneIdData? {
        val ies = scanResult.informationElements ?: return null

        for (ie in ies) {
            if (ie.id != IE_VENDOR_SPECIFIC) continue

            val bytes = ie.bytes ?: continue
            val payload = ByteArray(bytes.remaining())
            bytes.get(payload)
            bytes.rewind()

            // Check for DJI OUI at the start of the payload
            if (payload.size < 3) continue
            if (payload[0] != DJI_OUI[0] || payload[1] != DJI_OUI[1] || payload[2] != DJI_OUI[2]) continue

            // Strip OUI prefix
            val data = payload.copyOfRange(3, payload.size)
            if (data.size < MIN_PAYLOAD_LENGTH) {
                Log.d(TAG, "DroneID IE too short: ${data.size} bytes (need $MIN_PAYLOAD_LENGTH)")
                continue
            }

            return parsePayload(data)
        }

        return null
    }

    /**
     * Parse the DroneID payload bytes (after OUI) into structured data.
     */
    private fun parsePayload(data: ByteArray): DjiDroneIdData? {
        val version = data[0].toInt() and 0xFF

        // Drone position: degrees × 1e7 as signed 32-bit integer
        val droneLon = readInt32LE(data, 5) / 1e7
        val droneLat = readInt32LE(data, 9) / 1e7

        // Sanity check coordinates
        if (droneLat < -90 || droneLat > 90 || droneLon < -180 || droneLon > 180) {
            Log.d(TAG, "Invalid coordinates in DroneID: lat=$droneLat, lon=$droneLon")
            return null
        }
        if (droneLat == 0.0 && droneLon == 0.0) {
            Log.d(TAG, "Zero coordinates in DroneID — GPS not acquired")
            return null
        }

        // Altitude relative to takeoff (int16, meters)
        val altitudeRelative = readInt16LE(data, 13).toDouble()

        // Height above ground (int16, decimeters → meters)
        val heightAboveGround = readInt16LE(data, 15) / 10.0

        // Speed (uint16, cm/s → m/s)
        val speedCmS = readUInt16LE(data, 17)
        val speedMps = speedCmS / 100.0f

        // Heading (int16, degrees × 100 → degrees)
        val headingRaw = readInt16LE(data, 19)
        val headingDeg = headingRaw / 100.0f

        // Home point / operator position
        val homeLon = readInt32LE(data, 21) / 1e7
        val homeLat = readInt32LE(data, 25) / 1e7

        // Try to extract serial number (ASCII bytes starting at offset 1)
        val serial = try {
            val serialBytes = data.copyOfRange(1, 5)
            String(serialBytes, Charsets.US_ASCII).trim('\u0000')
        } catch (_: Exception) {
            null
        }

        Log.d(TAG, "Parsed DroneID: lat=$droneLat, lon=$droneLon, alt=${altitudeRelative}m, " +
                "speed=${speedMps}m/s, heading=${headingDeg}°, serial=$serial")

        return DjiDroneIdData(
            latitude = droneLat,
            longitude = droneLon,
            altitudeMeters = altitudeRelative,
            heightAboveGroundMeters = heightAboveGround,
            speedMps = speedMps,
            headingDegrees = headingDeg,
            homeLatitude = if (homeLat != 0.0) homeLat else null,
            homeLongitude = if (homeLon != 0.0) homeLon else null,
            serialPrefix = serial,
            version = version
        )
    }

    /** Read a signed 32-bit little-endian integer at the given offset. */
    private fun readInt32LE(data: ByteArray, offset: Int): Int {
        if (offset + 3 >= data.size) return 0
        return (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8) or
                ((data[offset + 2].toInt() and 0xFF) shl 16) or
                ((data[offset + 3].toInt() and 0xFF) shl 24)
    }

    /** Read a signed 16-bit little-endian integer at the given offset. */
    private fun readInt16LE(data: ByteArray, offset: Int): Int {
        if (offset + 1 >= data.size) return 0
        val value = (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8)
        // Sign extend from 16-bit
        return if (value >= 0x8000) value - 0x10000 else value
    }

    /** Read an unsigned 16-bit little-endian integer at the given offset. */
    private fun readUInt16LE(data: ByteArray, offset: Int): Int {
        if (offset + 1 >= data.size) return 0
        return (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8)
    }
}

/**
 * Parsed DJI DroneID telemetry from a WiFi beacon vendor-specific IE.
 *
 * @property latitude Drone latitude (WGS84 degrees)
 * @property longitude Drone longitude (WGS84 degrees)
 * @property altitudeMeters Altitude relative to takeoff point (meters)
 * @property heightAboveGroundMeters Height above ground (meters)
 * @property speedMps Ground speed (m/s)
 * @property headingDegrees Heading (0-360, true north)
 * @property homeLatitude Operator/home point latitude, null if not available
 * @property homeLongitude Operator/home point longitude, null if not available
 * @property serialPrefix First 4 characters of serial number, null if not readable
 * @property version DroneID protocol version byte
 */
data class DjiDroneIdData(
    val latitude: Double,
    val longitude: Double,
    val altitudeMeters: Double,
    val heightAboveGroundMeters: Double,
    val speedMps: Float,
    val headingDegrees: Float,
    val homeLatitude: Double?,
    val homeLongitude: Double?,
    val serialPrefix: String?,
    val version: Int
)
