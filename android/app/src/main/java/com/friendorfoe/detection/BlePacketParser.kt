package com.friendorfoe.detection

import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanResult
import android.os.Build
import android.os.ParcelUuid
import java.util.UUID
import kotlin.math.cos
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Parses rich detail from BLE advertisement packets.
 *
 * Extracts battery levels, separated-from-owner flags, device models,
 * broadcast URLs, distance estimates, and address types from raw
 * manufacturer data and service data fields.
 */
object BlePacketParser {

    private const val APPLE_CID = 0x004C
    private const val AIRTAG_TYPE = 0x12
    private const val AIRPODS_TYPE = 0x07
    private const val IBEACON_TYPE = 0x02
    private const val NEARBY_TYPE = 0x10
    private const val EDDYSTONE_URL_FRAME = 0x10
    private const val EDDYSTONE_TLM_FRAME = 0x20

    private val UUID_FD5A = ParcelUuid.fromString("0000FD5A-0000-1000-8000-00805F9B34FB")
    private val UUID_FE2C = ParcelUuid.fromString("0000FE2C-0000-1000-8000-00805F9B34FB")
    private val UUID_FEAA = ParcelUuid.fromString("0000FEAA-0000-1000-8000-00805F9B34FB")

    // ── Data classes ──

    enum class BatteryLevel { FULL, MEDIUM, LOW, CRITICAL, UNKNOWN }
    enum class AirPodsFamily { AIRPODS, AIRPODS_PRO, BEATS, UNKNOWN }
    enum class SmartTagState { PREMATURE_OFFLINE, OFFLINE, OVERMATURE, CONNECTED, UNKNOWN }
    enum class AddressType { PUBLIC, RANDOM, UNKNOWN, UNAVAILABLE }

    data class AirTagInfo(
        val batteryLevel: BatteryLevel,
        val separated: Boolean,
        val rawStatus: Int
    )

    data class AirPodsInfo(
        val modelId: Int,
        val family: AirPodsFamily,
        val batteryLeftPct: Int?,
        val batteryRightPct: Int?,
        val batteryCasePct: Int?
    )

    data class SmartTagInfo(
        val state: SmartTagState,
        val agingCounter: Int,
        val agingMinutes: Long,
        val batteryLevel: BatteryLevel
    )

    data class FastPairInfo(val modelId: Int)

    data class IBeaconInfo(
        val uuid: UUID,
        val major: Int,
        val minor: Int,
        val txPower: Int
    )

    data class EddystoneUrlInfo(val url: String, val txPower: Int)

    data class EddystoneTlmInfo(
        val batteryMillivolts: Int,
        val temperatureC: Float,
        val advertCount: Long,
        val uptimeSeconds: Long
    )

    // ── Model maps ──

    private val airPodsModels = mapOf(
        0x02 to AirPodsFamily.AIRPODS,
        0x03 to AirPodsFamily.AIRPODS,
        0x0F to AirPodsFamily.AIRPODS,
        0x0E to AirPodsFamily.AIRPODS_PRO,
        0x14 to AirPodsFamily.AIRPODS_PRO,
        0x09 to AirPodsFamily.BEATS,
        0x0B to AirPodsFamily.BEATS,
        0x0D to AirPodsFamily.BEATS,
    )

    private val urlSchemes = arrayOf("http://www.", "https://www.", "http://", "https://")
    private val urlEncodings = arrayOf(
        ".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/",
        ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"
    )

    // ── Parsers ──

    /** Parse Apple AirTag / FindMy accessory (type 0x12) */
    fun parseAirTag(result: ScanResult): AirTagInfo? {
        val mfg = result.scanRecord?.getManufacturerSpecificData(APPLE_CID) ?: return null
        if (mfg.size < 3 || u(mfg[0]) != AIRTAG_TYPE) return null
        val status = u(mfg[2])
        val batteryBits = (status shr 6) and 0x03
        val separated = (status and 0x20) != 0
        return AirTagInfo(batteryFromBits(batteryBits), separated, status)
    }

    /** Parse Apple AirPods / Beats (type 0x07) */
    fun parseAirPods(result: ScanResult): AirPodsInfo? {
        val mfg = result.scanRecord?.getManufacturerSpecificData(APPLE_CID) ?: return null
        if (mfg.size < 3 || u(mfg[0]) != AIRPODS_TYPE) return null
        val modelId = u(mfg[2])
        val family = airPodsModels[modelId] ?: AirPodsFamily.UNKNOWN

        // Try common battery nibble offsets
        val offsets = listOf(6 to 7, 7 to 8, 10 to 11)
        val pair = offsets.firstOrNull { (lr, c) ->
            mfg.size > c && areBatteryNibblesPlausible(u(mfg[lr]), u(mfg[c]))
        }
        if (pair == null) return AirPodsInfo(modelId, family, null, null, null)

        val (lrOff, caseOff) = pair
        val lr = u(mfg[lrOff])
        val caseByte = u(mfg[caseOff])
        return AirPodsInfo(
            modelId, family,
            decodeBatteryNibble((lr shr 4) and 0x0F),
            decodeBatteryNibble(lr and 0x0F),
            decodeBatteryNibble((caseByte shr 4) and 0x0F)
        )
    }

    /** Parse iBeacon (Apple type 0x02) */
    fun parseIBeacon(result: ScanResult): IBeaconInfo? {
        val mfg = result.scanRecord?.getManufacturerSpecificData(APPLE_CID) ?: return null
        if (mfg.size < 23 || u(mfg[0]) != IBEACON_TYPE || u(mfg[1]) != 0x15) return null
        val uuidBytes = mfg.copyOfRange(2, 18)
        val uuid = bytesToUUID(uuidBytes)
        val major = (u(mfg[18]) shl 8) or u(mfg[19])
        val minor = (u(mfg[20]) shl 8) or u(mfg[21])
        val txPower = mfg[22].toInt() // signed
        return IBeaconInfo(uuid, major, minor, txPower)
    }

    /** Parse Samsung SmartTag (service data UUID 0xFD5A) */
    fun parseSmartTag(result: ScanResult): SmartTagInfo? {
        val sd = result.scanRecord?.getServiceData(UUID_FD5A) ?: return null
        if (sd.size < 13) return null
        val stateBits = (u(sd[0]) shr 5) and 0x07
        val state = when (stateBits) {
            0x01 -> SmartTagState.PREMATURE_OFFLINE
            0x02 -> SmartTagState.OFFLINE
            0x03 -> SmartTagState.OVERMATURE
            0x05 -> SmartTagState.CONNECTED
            else -> SmartTagState.UNKNOWN
        }
        val aging = (u(sd[1]) shl 16) or (u(sd[2]) shl 8) or u(sd[3])
        val batteryBits = (u(sd[12]) shr 6) and 0x03
        return SmartTagInfo(state, aging, aging.toLong() * 15L, batteryFromBits(batteryBits))
    }

    /** Parse Google Fast Pair (service data UUID 0xFE2C) */
    fun parseFastPair(result: ScanResult): FastPairInfo? {
        val sd = result.scanRecord?.getServiceData(UUID_FE2C) ?: return null
        if (sd.size < 3) return null
        val modelId = (u(sd[0]) shl 16) or (u(sd[1]) shl 8) or u(sd[2])
        return FastPairInfo(modelId)
    }

    /** Parse Eddystone-URL (service data UUID 0xFEAA, frame 0x10) */
    fun parseEddystoneUrl(result: ScanResult): EddystoneUrlInfo? {
        val sd = result.scanRecord?.getServiceData(UUID_FEAA) ?: return null
        if (sd.size < 3 || u(sd[0]) != EDDYSTONE_URL_FRAME) return null
        val txPower = sd[1].toInt()
        val schemeIdx = u(sd[2])
        if (schemeIdx >= urlSchemes.size) return null
        val sb = StringBuilder(urlSchemes[schemeIdx])
        for (i in 3 until sd.size) {
            val b = u(sd[i])
            if (b < urlEncodings.size) sb.append(urlEncodings[b]) else sb.append(b.toChar())
        }
        return EddystoneUrlInfo(sb.toString(), txPower)
    }

    /** Parse Eddystone-TLM (service data UUID 0xFEAA, frame 0x20) */
    fun parseEddystoneTlm(result: ScanResult): EddystoneTlmInfo? {
        val sd = result.scanRecord?.getServiceData(UUID_FEAA) ?: return null
        if (sd.size < 14 || u(sd[0]) != EDDYSTONE_TLM_FRAME) return null
        val batteryMv = (u(sd[2]) shl 8) or u(sd[3])
        val tempWhole = sd[4].toInt() // signed
        val tempFrac = u(sd[5])
        val temp = tempWhole + tempFrac / 256f
        val pduCount = (u(sd[6]).toLong() shl 24) or (u(sd[7]).toLong() shl 16) or
                (u(sd[8]).toLong() shl 8) or u(sd[9]).toLong()
        val uptime = ((u(sd[10]).toLong() shl 24) or (u(sd[11]).toLong() shl 16) or
                (u(sd[12]).toLong() shl 8) or u(sd[13]).toLong()) / 10L
        return EddystoneTlmInfo(batteryMv, temp, pduCount, uptime)
    }

    /** Estimate distance from TX Power and RSSI */
    fun estimateDistanceMeters(result: ScanResult, pathLossExp: Double = 2.0): Double? {
        val txPower = result.txPower
        if (txPower == Int.MIN_VALUE) return null
        val ratioDb = txPower - result.rssi
        return 10.0.pow(ratioDb / (10.0 * pathLossExp))
    }

    /** Estimate distance from explicit TX Power value */
    fun estimateDistanceMeters(txPower: Int, rssi: Int, pathLossExp: Double = 2.0): Double {
        return 10.0.pow((txPower - rssi) / (10.0 * pathLossExp))
    }

    /** Get BLE address type (API 33+) */
    @Suppress("MissingPermission")
    fun getAddressType(result: ScanResult): AddressType {
        return if (Build.VERSION.SDK_INT >= 33) {
            when (result.device.addressType) {
                BluetoothDevice.ADDRESS_TYPE_PUBLIC -> AddressType.PUBLIC
                BluetoothDevice.ADDRESS_TYPE_RANDOM -> AddressType.RANDOM
                else -> AddressType.UNKNOWN
            }
        } else {
            // Heuristic: check MSB of MAC address
            // Random addresses have bits 7:6 = 01 (resolvable) or 11 (non-resolvable)
            val macFirst = result.device.address.substringBefore(":").uppercase()
            try {
                val firstByte = macFirst.toInt(16)
                if ((firstByte and 0xC0) != 0x00) AddressType.RANDOM else AddressType.PUBLIC
            } catch (_: Exception) {
                AddressType.UNAVAILABLE
            }
        }
    }

    /**
     * Get Apple device type from manufacturer data type byte.
     * Returns human-readable type or null if not Apple.
     */
    fun getAppleDeviceType(result: ScanResult): String? {
        val mfg = result.scanRecord?.getManufacturerSpecificData(APPLE_CID) ?: return null
        if (mfg.isEmpty()) return null
        return when (u(mfg[0])) {
            0x02 -> "iBeacon"
            0x05 -> "AirDrop"
            0x07 -> "AirPods/Beats"
            0x09 -> "AirPlay Target"
            0x10 -> "iPhone/iPad/Mac Nearby"
            0x12 -> "AirTag/FindMy"
            else -> "Apple Device (0x${u(mfg[0]).toString(16)})"
        }
    }

    /**
     * Parse all available details from a scan result into a key-value map.
     * Tries all parsers and returns whatever data is available.
     */
    fun parseAllDetails(result: ScanResult): Map<String, String> {
        val details = mutableMapOf<String, String>()

        // Apple AirTag
        parseAirTag(result)?.let { at ->
            details["Battery"] = at.batteryLevel.name.lowercase().replaceFirstChar { it.uppercase() }
            details["Separated"] = if (at.separated) "YES" else "Near owner"
        }

        // Apple AirPods
        parseAirPods(result)?.let { ap ->
            details["Model"] = ap.family.name.replace("_", " ")
            ap.batteryLeftPct?.let { details["L Batt"] = "$it%" }
            ap.batteryRightPct?.let { details["R Batt"] = "$it%" }
            ap.batteryCasePct?.let { details["Case"] = "$it%" }
        }

        // Samsung SmartTag
        parseSmartTag(result)?.let { st ->
            details["State"] = st.state.name.replace("_", " ").lowercase()
                .replaceFirstChar { it.uppercase() }
            details["Battery"] = st.batteryLevel.name.lowercase().replaceFirstChar { it.uppercase() }
            if (st.agingMinutes > 0) {
                val hours = st.agingMinutes / 60
                val mins = st.agingMinutes % 60
                details["Offline"] = if (hours > 0) "${hours}h ${mins}m" else "${mins}m"
            }
        }

        // iBeacon
        parseIBeacon(result)?.let { ib ->
            details["Beacon UUID"] = ib.uuid.toString().take(8) + "..."
            details["Major/Minor"] = "${ib.major}/${ib.minor}"
        }

        // Google Fast Pair
        parseFastPair(result)?.let { fp ->
            details["Fast Pair ID"] = "0x${fp.modelId.toString(16).uppercase().padStart(6, '0')}"
        }

        // Eddystone URL
        parseEddystoneUrl(result)?.let { eu ->
            details["Beacon URL"] = eu.url
        }

        // Eddystone TLM
        parseEddystoneTlm(result)?.let { tlm ->
            if (tlm.batteryMillivolts > 0) details["Beacon Batt"] = "${tlm.batteryMillivolts}mV"
            details["Beacon Temp"] = "${"%.1f".format(tlm.temperatureC)}°C"
            details["Uptime"] = "${tlm.uptimeSeconds / 3600}h"
        }

        // Distance estimate
        estimateDistanceMeters(result)?.let { dist ->
            details["Est. Dist"] = "${"%.1f".format(dist)}m"
        }

        // Address type
        val addrType = getAddressType(result)
        if (addrType != AddressType.UNAVAILABLE) {
            details["Address"] = addrType.name.lowercase().replaceFirstChar { it.uppercase() }
        }

        return details
    }

    // ── Helpers ──

    private fun u(b: Byte): Int = b.toInt() and 0xFF

    private fun batteryFromBits(bits: Int): BatteryLevel = when (bits and 0x03) {
        0x00 -> BatteryLevel.FULL
        0x01 -> BatteryLevel.MEDIUM
        0x02 -> BatteryLevel.LOW
        0x03 -> BatteryLevel.CRITICAL
        else -> BatteryLevel.UNKNOWN
    }

    private fun decodeBatteryNibble(nibble: Int): Int? {
        if (nibble == 0x0F || nibble > 10) return null
        return nibble * 10
    }

    private fun areBatteryNibblesPlausible(lrByte: Int, caseByte: Int): Boolean {
        val l = (lrByte shr 4) and 0x0F
        val r = lrByte and 0x0F
        val c = (caseByte shr 4) and 0x0F
        return (l == 0x0F || l in 0..10) && (r == 0x0F || r in 0..10) && (c == 0x0F || c in 0..10)
    }

    private fun bytesToUUID(bytes: ByteArray): UUID {
        var msb = 0L
        var lsb = 0L
        for (i in 0..7) msb = (msb shl 8) or (bytes[i].toLong() and 0xFF)
        for (i in 8..15) lsb = (lsb shl 8) or (bytes[i].toLong() and 0xFF)
        return UUID(msb, lsb)
    }
}
