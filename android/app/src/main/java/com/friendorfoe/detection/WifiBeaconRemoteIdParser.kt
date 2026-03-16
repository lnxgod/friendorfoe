package com.friendorfoe.detection

import android.net.wifi.ScanResult
import android.os.Build
import android.util.Log

/**
 * Parser for ASTM F3411 Remote ID messages broadcast via WiFi beacon
 * vendor-specific Information Elements.
 *
 * Drones compliant with FAA Remote ID may broadcast OpenDroneID messages
 * in WiFi beacon frames using OUI FA:0B:BC (ASTM International) with
 * OUI type 0x0D. After the 4-byte header (3-byte OUI + 1-byte type),
 * the payload contains a 1-byte message counter followed by N × 25-byte
 * OpenDroneID messages, identical in wire format to BLE and WiFi NaN
 * transports.
 *
 * This parser mirrors [DjiDroneIdParser]'s IE extraction approach but
 * delegates message parsing to [OpenDroneIdParser.parseMessage] for
 * stateful accumulation across scans.
 *
 * Requires API 33+ for [ScanResult.getInformationElements].
 */
object WifiBeaconRemoteIdParser {

    private const val TAG = "WifiBeaconRidParser"

    /** ASTM International OUI for Remote ID (FA:0B:BC) */
    private val ASTM_OUI = byteArrayOf(0xFA.toByte(), 0x0B, 0xBC.toByte())

    /** OUI type byte for OpenDroneID */
    private const val OUI_TYPE_ODID: Byte = 0x0D

    /** Vendor-specific IE element ID per IEEE 802.11 */
    private const val IE_VENDOR_SPECIFIC = 221

    /** Each OpenDroneID message is exactly 25 bytes */
    private const val ODID_MSG_SIZE = 25

    /** Minimum IE payload: 3 (OUI) + 1 (type) + 1 (counter) + 25 (one message) = 30 */
    private const val MIN_PAYLOAD_SIZE = 30

    /**
     * Attempt to parse ASTM F3411 WiFi Beacon Remote ID messages from a scan result.
     *
     * @param scanResult WiFi scan result to inspect for ASTM vendor IEs
     * @param state Mutable accumulator for partial drone state
     * @return true if at least one OpenDroneID message was parsed
     */
    fun parse(scanResult: ScanResult, state: OpenDroneIdParser.DronePartialState): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return false
        }

        return try {
            parseApi33(scanResult, state)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse WiFi Beacon RID IE: ${e.message}")
            false
        }
    }

    @Suppress("NewApi")
    private fun parseApi33(
        scanResult: ScanResult,
        state: OpenDroneIdParser.DronePartialState
    ): Boolean {
        val ies = scanResult.informationElements ?: return false
        var parsed = false

        for (ie in ies) {
            if (ie.id != IE_VENDOR_SPECIFIC) continue

            val bytes = ie.bytes ?: continue
            val payload = ByteArray(bytes.remaining())
            bytes.get(payload)
            bytes.rewind()

            if (payload.size < MIN_PAYLOAD_SIZE) continue

            // Check ASTM OUI
            if (payload[0] != ASTM_OUI[0] ||
                payload[1] != ASTM_OUI[1] ||
                payload[2] != ASTM_OUI[2]
            ) continue

            // Check OUI type
            if (payload[3] != OUI_TYPE_ODID) continue

            // Byte 4 is the message counter (number of 25-byte messages)
            val messageCount = payload[4].toInt() and 0xFF
            val dataStart = 5
            val availableBytes = payload.size - dataStart

            // Parse each 25-byte OpenDroneID message
            val actualCount = minOf(messageCount, availableBytes / ODID_MSG_SIZE)
            for (i in 0 until actualCount) {
                val offset = dataStart + i * ODID_MSG_SIZE
                val msgData = payload.copyOfRange(offset, offset + ODID_MSG_SIZE)
                OpenDroneIdParser.parseMessage(msgData, state)
                parsed = true
            }

            if (parsed) {
                Log.d(TAG, "Parsed $actualCount ASTM F3411 WiFi Beacon RID message(s) " +
                        "from BSSID=${scanResult.BSSID}")
            }
        }

        return parsed
    }
}
