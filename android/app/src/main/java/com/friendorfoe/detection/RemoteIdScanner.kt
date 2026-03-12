package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid
import android.util.Log
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/**
 * BLE Remote ID scanner for detecting compliant drones.
 *
 * Scans for OpenDroneID BLE advertisements as defined by ASTM F3411.
 * Parses Basic ID (message type 0) for drone serial number and
 * Location (message type 1) for position, altitude, heading, and speed.
 *
 * FAA Remote ID mandate effective March 2024 requires all drones >=250g
 * to broadcast identification and location via BLE or WiFi.
 *
 * Reference: OpenDroneID receiver-android library structure.
 */
@Singleton
class RemoteIdScanner @Inject constructor(
    private val bluetoothManager: BluetoothManager
) {

    companion object {
        private const val TAG = "RemoteIdScanner"

        /** OpenDroneID BLE service UUID per ASTM F3411-19 / F3411-22a */
        val OPEN_DRONE_ID_UUID: UUID = UUID.fromString("0000FFFA-0000-1000-8000-00805F9B34FB")

        // OpenDroneID message types
        private const val MSG_TYPE_BASIC_ID = 0
        private const val MSG_TYPE_LOCATION = 1
        private const val MSG_TYPE_AUTH = 2
        private const val MSG_TYPE_SELF_ID = 3
        private const val MSG_TYPE_SYSTEM = 4
        private const val MSG_TYPE_OPERATOR_ID = 5

        // Location message field constants
        private const val LAT_LON_SCALE = 1e-7
        private const val ALT_SCALE = 0.5
        private const val ALT_OFFSET = -1000.0
        private const val SPEED_SCALE = 0.25f
        private const val HEADING_SCALE = (360.0f / 256.0f) // Direction encoded in 1 byte
    }

    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var bleScanner: BluetoothLeScanner? = null

    // Track parsed data per drone MAC address. We may receive Basic ID and Location
    // in separate advertisements, so we accumulate partial state.
    private val droneStates = mutableMapOf<String, DronePartialState>()

    /**
     * Start scanning for Remote ID broadcasts.
     *
     * Returns a Flow that emits Drone objects as they are detected via BLE.
     * If BLE is unavailable or disabled, the flow completes without emitting.
     *
     * Requires BLUETOOTH_SCAN permission (Android 12+) or BLUETOOTH + ACCESS_FINE_LOCATION (older).
     */
    @SuppressLint("MissingPermission")
    fun startScanning(): Flow<Drone> = callbackFlow {
        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled) {
            Log.w(TAG, "Bluetooth not available or disabled, skipping Remote ID scan")
            close()
            return@callbackFlow
        }

        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            Log.w(TAG, "BLE scanner not available")
            close()
            return@callbackFlow
        }
        bleScanner = scanner

        val scanFilter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(OPEN_DRONE_ID_UUID))
            .build()

        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setReportDelay(0) // Immediate callback per result
            .build()

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                try {
                    val drone = processScanResult(result)
                    if (drone != null) {
                        trySend(drone)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing BLE scan result", e)
                }
            }

            override fun onBatchScanResults(results: List<ScanResult>) {
                for (result in results) {
                    try {
                        val drone = processScanResult(result)
                        if (drone != null) {
                            trySend(drone)
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error processing batch BLE scan result", e)
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "BLE scan failed with error code: $errorCode")
                // Don't close the flow on scan failure; the scanner may recover
            }
        }

        Log.i(TAG, "Starting BLE Remote ID scan")
        scanner.startScan(listOf(scanFilter), scanSettings, callback)

        awaitClose {
            Log.i(TAG, "Stopping BLE Remote ID scan")
            try {
                scanner.stopScan(callback)
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping BLE scan", e)
            }
            droneStates.clear()
        }
    }

    /** Stop scanning (for imperative callers; flow-based callers cancel the coroutine). */
    @SuppressLint("MissingPermission")
    fun stopScanning() {
        try {
            // The actual stop is handled in callbackFlow's awaitClose.
            // This method exists for imperative lifecycle management.
            bleScanner = null
            droneStates.clear()
        } catch (e: Exception) {
            Log.w(TAG, "Error in stopScanning", e)
        }
    }

    /**
     * Process a BLE scan result containing OpenDroneID service data.
     *
     * Returns a fully-formed Drone if we have both Basic ID and Location data,
     * or a partial Drone with just the serial if we only have Basic ID so far.
     */
    private fun processScanResult(result: ScanResult): Drone? {
        val deviceAddress = result.device.address
        val serviceData = result.scanRecord?.getServiceData(ParcelUuid(OPEN_DRONE_ID_UUID))
            ?: return null

        if (serviceData.isEmpty()) return null

        // OpenDroneID BLE: first byte of service data is the App Code (0x0D for OpenDroneID),
        // followed by the message counter byte, then the 25-byte message.
        // Some implementations put the message type as the first nibble of byte 0 of the message.
        // We handle both formats by checking data length.
        val messageData = if (serviceData.size >= 27) {
            // App Code (1 byte) + Counter (1 byte) + 25-byte message
            serviceData.copyOfRange(2, serviceData.size)
        } else if (serviceData.size >= 25) {
            // Raw 25-byte message (some implementations)
            serviceData
        } else {
            Log.d(TAG, "Service data too short: ${serviceData.size} bytes")
            return null
        }

        val messageType = (messageData[0].toInt() and 0xF0) ushr 4
        val now = Instant.now()

        val state = droneStates.getOrPut(deviceAddress) {
            DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
        }
        state.lastUpdated = now
        state.signalStrengthDbm = result.rssi

        when (messageType) {
            MSG_TYPE_BASIC_ID -> parseBasicId(messageData, state)
            MSG_TYPE_LOCATION -> parseLocation(messageData, state)
            MSG_TYPE_SYSTEM -> parseSystem(messageData, state)
            MSG_TYPE_OPERATOR_ID -> parseOperatorId(messageData, state)
            else -> Log.d(TAG, "Unhandled OpenDroneID message type: $messageType")
        }

        return state.toDroneOrNull()
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

        // Extract UAS ID (serial number), trimming null bytes
        val serialBytes = data.copyOfRange(2, 22)
        val serial = String(serialBytes, Charsets.US_ASCII).trimEnd('\u0000').trim()

        if (serial.isNotEmpty()) {
            state.droneId = serial
        }

        // UA type can hint at manufacturer, but the spec doesn't define manufacturer fields
        state.uaType = uaType
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

        // Byte 2: Direction/heading (value * 2 = degrees)
        val directionRaw = data[2].toInt() and 0xFF
        val heading = directionRaw * 2.0f

        // Byte 3: Horizontal speed (value * 0.25 = m/s)
        val speedRaw = data[3].toInt() and 0xFF
        val speedMps = speedRaw * SPEED_SCALE

        // Bytes 5-8: Latitude (int32 * 1e-7)
        val latRaw = buffer.getInt(5)
        val latitude = latRaw * LAT_LON_SCALE

        // Bytes 9-12: Longitude (int32 * 1e-7)
        val lonRaw = buffer.getInt(9)
        val longitude = lonRaw * LAT_LON_SCALE

        // Bytes 13-14: Pressure altitude (uint16 * 0.5 - 1000)
        val altRaw = buffer.getShort(13).toInt() and 0xFFFF
        val altitudeMeters = altRaw * ALT_SCALE + ALT_OFFSET

        // Validate coordinates - 0,0 or extreme values indicate invalid data
        if (latitude == 0.0 && longitude == 0.0) return
        if (latitude < -90.0 || latitude > 90.0) return
        if (longitude < -180.0 || longitude > 180.0) return

        state.latitude = latitude
        state.longitude = longitude
        state.altitudeMeters = altitudeMeters
        state.heading = if (heading <= 360f) heading else null
        state.speedMps = speedMps
    }

    /**
     * Parse System message (Type 4) - contains operator location.
     *
     * Format:
     *   Byte 1: Operator location type
     *   Bytes 2-5: Operator latitude (int32 * 1e-7)
     *   Bytes 6-9: Operator longitude (int32 * 1e-7)
     *   ... (remaining fields: area count, radius, ceiling, floor, etc.)
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
     * Mutable accumulator for partial drone state.
     *
     * BLE advertisements may deliver Basic ID and Location in separate packets,
     * so we accumulate data per device address until we have enough to emit a Drone.
     */
    private data class DronePartialState(
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
        var signalStrengthDbm: Int? = null
    ) {
        /**
         * Convert to a Drone domain object if we have minimum required data.
         * At minimum we need a drone ID (from Basic ID) or we use the device address.
         * If we have location data, we include it; otherwise we use 0,0,0 as placeholder.
         */
        fun toDroneOrNull(): Drone? {
            val id = droneId ?: return null
            val lat = latitude
            val lon = longitude
            val alt = altitudeMeters

            // Require at least a valid position to emit
            if (lat == null || lon == null || alt == null) return null

            return Drone(
                id = "rid_$id",
                position = Position(
                    latitude = lat,
                    longitude = lon,
                    altitudeMeters = alt,
                    heading = heading,
                    speedMps = speedMps
                ),
                source = DetectionSource.REMOTE_ID,
                category = ObjectCategory.DRONE,
                confidence = 0.9f,
                firstSeen = firstSeen,
                lastUpdated = lastUpdated,
                droneId = id,
                operatorLatitude = operatorLatitude,
                operatorLongitude = operatorLongitude,
                signalStrengthDbm = signalStrengthDbm
            )
        }
    }
}
