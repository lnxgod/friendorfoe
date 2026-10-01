package com.friendorfoe.data.probes

import com.friendorfoe.data.remote.ProbeActivityDto
import com.friendorfoe.data.remote.ProbeObserverDto
import com.friendorfoe.data.remote.ProbeTargetDto
import com.friendorfoe.data.remote.ProbeTransmitterDto
import com.google.gson.JsonParser
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.Locale

const val PROBE_USB_VENDOR = 0x303a
const val PROBE_USB_PRODUCT = 0x4001
const val PROBE_USB_NAME = "FoF WiFi Probe"
const val PROBE_FLASHER_URL = "https://lnxgod.github.io/friendorfoe/#usb-probe"

internal fun isUsbProbeProduct(vendor: Int, product: Int, name: String?): Boolean =
    vendor == PROBE_USB_VENDOR && product == PROBE_USB_PRODUCT && name == PROBE_USB_NAME

/** Bytes, not characters: USB reads may split a UTF-8 code point or a JSON line. */
internal class UsbProbeLineFramer {
    private val pending = ByteArray(512)
    private var used = 0
    private var overflow = false
    fun accept(bytes: ByteArray, count: Int): List<String> = buildList {
        for (i in 0 until count.coerceIn(0, bytes.size)) {
            val byte = bytes[i]
            if (byte == 10.toByte()) {
                if (!overflow && used > 0) runCatching {
                    Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(pending, 0, used)).toString().trimEnd('\r')
                }.getOrNull()?.let(::add)
                used = 0; overflow = false
            } else if (!overflow) {
                if (used == pending.size) { overflow = true; used = 0 }
                else pending[used++] = byte
            }
        }
    }
}

/** A connection-scoped, bounded five-minute buffer. Never combines identities across boots. */
internal class UsbProbeSession {
    var sensorId: String? = null; private set
    var version = ""; private set
    var lastHeartbeatMs: Long? = null; private set
    var dropped = 0L; private set
    private var boot = ""
    private var sequence = -1L
    private var deviceMs = 0L
    private data class Report(val mac: String, val ssid: String, val wildcard: Boolean,
        val binary: Boolean, val rssi: Int, val channel: Int, val receivedMs: Long, val wallSeconds: Double)
    private val reports = ArrayDeque<Report>()

    fun accept(line: String, nowMs: Long, wallSeconds: Double): Boolean = runCatching {
        val obj = JsonParser.parseString(line).asJsonObject
        fun string(key: String) = obj[key].takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }!!.asString
        fun number(key: String): Long {
            val value = obj[key].takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isNumber }!!.asBigDecimal
            return value.longValueExact()
        }
        fun bool(key: String) = obj[key].takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isBoolean }!!.asBoolean
        if (number("protocol") != 1L) return false
        val nextBoot = string("boot")
        if (!nextBoot.matches(Regex("[0-9a-f]{16}"))) return false
        val uptime = number("uptime_ms")
        if (uptime < 0) return false
        when (string("type")) {
            "hello" -> {
                if (string("firmware") != "fof-wifi-probe") return false
                val sensor = string("sensor_id")
                val firmwareVersion = string("version")
                val drops = number("dropped")
                if (!sensor.matches(Regex("usb-[0-9a-f]{12}")) || firmwareVersion.length !in 1..32 || drops < 0) return false
                if (boot != nextBoot || sensorId != sensor) { reports.clear(); sequence = -1 }
                else if (uptime < deviceMs) return false
                boot = nextBoot; sensorId = sensor; version = firmwareVersion
                deviceMs = uptime; lastHeartbeatMs = nowMs; dropped = drops
                true
            }
            "probe" -> {
                val heartbeat = lastHeartbeatMs ?: return false
                if (nextBoot != boot || nowMs - heartbeat !in 0..8_000 || uptime < deviceMs - 2000 || uptime > deviceMs + 8000) return false
                val seq = number("seq")
                if (seq <= sequence) return false
                val mac = string("mac").lowercase(Locale.ROOT)
                if (!mac.matches(Regex("(?:[0-9a-f]{2}:){5}[0-9a-f]{2}")) || mac == "00:00:00:00:00:00" || mac.take(2).toInt(16) and 1 != 0) return false
                val ssid = string("ssid")
                val wildcard = bool("wildcard"); val binary = bool("binary")
                if (ssid.toByteArray(Charsets.UTF_8).size > 32 || ssid.any { it.code < 32 || it.code == 127 } ||
                    (wildcard && binary) || ((wildcard || binary) != ssid.isEmpty())) return false
                val rssi = number("rssi"); val channel = number("channel")
                if (rssi !in -127..-1 || channel !in 1..13) return false
                sequence = seq
                reports.addLast(Report(mac, ssid, wildcard, binary, rssi.toInt(), channel.toInt(), nowMs, wallSeconds))
                while (reports.size > 6000) reports.removeFirst()
                true
            }
            else -> false
        }
    }.getOrDefault(false)

    fun snapshot(nowMs: Long): ProbeActivityDto {
        while (reports.firstOrNull()?.let { nowMs - it.receivedMs > 300_000 } == true) reports.removeFirst()
        val sensor = sensorId ?: return ProbeActivityDto()
        val rows = reports.groupBy { it.mac }.values.sortedByDescending { it.last().receivedMs }.take(500).map { group ->
            val latest = group.last()
            ProbeTransmitterDto(mac = latest.mac, sensorId = sensor,
                locallyAdministered = latest.mac.take(2).toInt(16) and 2 != 0,
                reports = group.size, wildcardReports = group.count { it.wildcard }, unknownReports = group.count { it.binary },
                lastSeen = latest.wallSeconds, ageSeconds = (nowMs - latest.receivedMs).coerceAtLeast(0) / 1000.0,
                rssi = latest.rssi, channel = latest.channel,
                targets = group.filter { !it.wildcard && !it.binary }.groupBy { it.ssid }.map { (ssid, targets) ->
                    ProbeTargetDto(ssid, targets.size, targets.last().wallSeconds)
                }.sortedByDescending { it.lastSeen })
        }
        return ProbeActivityDto(sensorId = sensor, observers = listOf(ProbeObserverDto(sensor,
            lastHeartbeatMs?.let { (nowMs - it).coerceAtLeast(0) / 1000.0 })), transmitters = rows)
    }
}
