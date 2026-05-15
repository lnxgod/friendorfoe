package com.friendorfoe.data.badge

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.util.concurrent.TimeUnit
import java.util.zip.CRC32

enum class BadgeUsbStatus {
    DISCONNECTED,
    PERMISSION_NEEDED,
    CONNECTING,
    AP_CONNECTED,
    CONNECTED,
    ERROR
}

data class BadgeUsbDetection(
    val id: String,
    val manufacturer: String,
    val source: Int,
    val confidence: Float,
    val rssi: Int
)

data class BadgeThreatCounts(
    val drone: Int = 0,
    val meta: Int = 0,
    val tracker: Int = 0,
    val wifiAnomaly: Int = 0,
    val ble: Int = 0,
    val other: Int = 0
)

data class BadgeThreatEntity(
    val label: String,
    val detail: String = "",
    val threatClass: String,
    val category: String = "",
    val code: String = "",
    val displayId: String = "",
    val score: Int,
    val evidenceQuality: Int = 0,
    val displayRank: Int = 0,
    val ageSeconds: Int,
    val lastSeenSeconds: Int = 0,
    val rssi: Int,
    val bestRssi: Int = 0,
    val events: Int,
    val seenCount: Int = 0,
    val groupCount: Int = 0,
    val proximityLevel: Int = 0,
    val stale: Boolean = false,
    val lat: Double? = null,
    val lon: Double? = null,
    val altitudeM: Float? = null,
    val operatorLat: Double? = null,
    val operatorLon: Double? = null,
    val operatorId: String? = null
)

data class BadgeReportingStatus(
    val networkMode: String = "off",
    val backendEnabled: Boolean = false,
    val networkTtlSeconds: Int = 0,
    val wifiSta: Boolean = false,
    val standalone: Boolean = true,
    val uploadsOk: Int = 0,
    val uploadsFail: Int = 0,
    val lastUploadAgeSeconds: Long? = null
)

data class BadgeScannerStatus(
    val slot: Int = -1,
    val uart: String = "",
    val connected: Boolean = false,
    val slotRole: String = "",
    val expectedScanProfile: String = "",
    val scanProfile: String = "",
    val roleAcked: Boolean = false,
    val health: String = "",
    val uartRawSeen: Boolean = false,
    val uartRawAgeSeconds: Long? = null,
    val uartJsonErrors: Int = 0,
    val commandRx: Int = 0,
    val commandLastAgeSeconds: Long? = null,
    val bleAdvSeen: Int = 0,
    val bleFpEmit: Int = 0,
    val bleMetaSeen: Int = 0,
    val bleTrackerSeen: Int = 0,
    val ridEmit: Int = 0,
    val privacySeen: Int = 0,
    val wifiTotalFrames: Int = 0,
    val wifiDroneSsidEmit: Int = 0,
    val wifiNotableSsidEmit: Int = 0,
    val wifiLastDroneSsid: String = "",
    val wifiLastNotableSsid: String = "",
    val displayPolicyHash: Long = 0,
    val displayPolicyAckHash: Long = 0,
    val filteredCounts: Map<String, Int> = emptyMap(),
    val firmwareState: String = "",
    val targetVersion: String = "",
    val otaState: String = "",
    val lastFirmwareError: String = ""
)

data class BadgeDisplayClassPolicy(
    val enabled: Boolean = true,
    val lane: String = "lower",
    val minProximity: String = "present",
    val priority: Int = 50
)

data class BadgeDisplayPolicy(
    val version: Int = 1,
    val classes: Map<String, BadgeDisplayClassPolicy> = defaultBadgeDisplayPolicyClasses()
) {
    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        add("classes", JsonObject().apply {
            classes.forEach { (key, config) ->
                add(key, JsonObject().apply {
                    addProperty("enabled", config.enabled)
                    addProperty("lane", config.lane)
                    addProperty("min_proximity", config.minProximity)
                    addProperty("priority", config.priority.coerceIn(0, 100))
                })
            }
        })
    }
}

data class BadgeDisplayPolicyClassInfo(
    val key: String,
    val label: String
)

val BadgeDisplayPolicyClasses = listOf(
    BadgeDisplayPolicyClassInfo("drone", "Drone"),
    BadgeDisplayPolicyClassInfo("meta", "Meta Glasses"),
    BadgeDisplayPolicyClassInfo("tracker", "Tracker"),
    BadgeDisplayPolicyClassInfo("wifi_attack", "WiFi Attack"),
    BadgeDisplayPolicyClassInfo("skimmer", "Skimmer"),
    BadgeDisplayPolicyClassInfo("camera", "Camera"),
    BadgeDisplayPolicyClassInfo("flock", "Flock/ALPR"),
    BadgeDisplayPolicyClassInfo("lock", "Lock"),
    BadgeDisplayPolicyClassInfo("hid", "BLE HID"),
    BadgeDisplayPolicyClassInfo("beacon", "Venue Beacon"),
    BadgeDisplayPolicyClassInfo("event_badge", "Event Badge"),
    BadgeDisplayPolicyClassInfo("auracast", "Auracast"),
    BadgeDisplayPolicyClassInfo("scanner_status", "Scanner Status")
)

fun defaultBadgeDisplayPolicyClasses(): Map<String, BadgeDisplayClassPolicy> = mapOf(
    "drone" to BadgeDisplayClassPolicy(true, "both", "present", 100),
    "meta" to BadgeDisplayClassPolicy(true, "both", "present", 95),
    "tracker" to BadgeDisplayClassPolicy(true, "lower", "near", 70),
    "wifi_attack" to BadgeDisplayClassPolicy(true, "both", "present", 90),
    "skimmer" to BadgeDisplayClassPolicy(true, "both", "near", 88),
    "camera" to BadgeDisplayClassPolicy(true, "lower", "near", 65),
    "flock" to BadgeDisplayClassPolicy(true, "both", "present", 85),
    "lock" to BadgeDisplayClassPolicy(true, "lower", "near", 55),
    "hid" to BadgeDisplayClassPolicy(true, "lower", "close", 45),
    "beacon" to BadgeDisplayClassPolicy(true, "lower", "near", 30),
    "event_badge" to BadgeDisplayClassPolicy(true, "lower", "near", 35),
    "auracast" to BadgeDisplayClassPolicy(true, "lower", "near", 20),
    "scanner_status" to BadgeDisplayClassPolicy(true, "lower", "present", 10)
)

fun defaultBadgeDisplayPolicy(): BadgeDisplayPolicy =
    BadgeDisplayPolicy(classes = defaultBadgeDisplayPolicyClasses())

fun badgeDisplayPolicyCommandJson(
    policy: BadgeDisplayPolicy,
    persist: Boolean = true
): JsonObject = JsonObject().apply {
    addProperty("cmd", "badge_display_policy")
    addProperty("persist", persist)
    add("policy", policy.toJsonObject())
}

data class BadgeFirmwareProgress(
    val kind: String = "",
    val ok: Boolean? = null,
    val uart: String = "",
    val stage: String = "",
    val percent: Int = 0,
    val bytes: Long = 0,
    val total: Long = 0,
    val error: String = ""
)

data class BadgeControlStatus(
    val version: String = "",
    val mode: String = "local_ap",
    val modeLabel: String = "Local AP",
    val threatScore: Float = 0f,
    val colorRgb565: Int = 0,
    val reporting: BadgeReportingStatus = BadgeReportingStatus(),
    val counts: BadgeThreatCounts = BadgeThreatCounts(),
    val entities: List<BadgeThreatEntity> = emptyList(),
    val scanners: List<BadgeScannerStatus> = emptyList(),
    val displayPolicy: BadgeDisplayPolicy = defaultBadgeDisplayPolicy(),
    val displayPolicyHash: Long = 0,
    val filteredCounts: Map<String, Int> = emptyMap()
)

data class BadgeUsbState(
    val status: BadgeUsbStatus = BadgeUsbStatus.DISCONNECTED,
    val deviceName: String? = null,
    val message: String = "Connect a FoF badge over USB-C",
    val lastLine: String? = null,
    val eventCount: Int = 0,
    val detections: List<BadgeUsbDetection> = emptyList(),
    val controlStatus: BadgeControlStatus? = null,
    val firmwareProgress: BadgeFirmwareProgress? = null
)

@Singleton
class BadgeUsbRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val usbManager: UsbManager,
    okHttpClient: OkHttpClient
) {
    companion object {
        private const val TAG = "BadgeUsbRepository"
        private const val ACTION_USB_PERMISSION = "com.friendorfoe.action.USB_BADGE_PERMISSION"
        private const val ESPRESSIF_VENDOR_ID = 0x303A
        private const val BADGE_AP_BASE_URL = "http://192.168.4.1"
        private const val READ_TIMEOUT_MS = 250
        private const val WRITE_TIMEOUT_MS = 250
        private const val AP_POLL_INTERVAL_MS = 2500L
        private const val MAX_RECENT_DETECTIONS = 20
        private const val MAX_LINE_CHARS = 8192
        private const val FW_CHUNK_BYTES = 1024
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val connectionMutex = Mutex()
    private val badgeHttpClient = okHttpClient.newBuilder()
        .connectTimeout(1200, TimeUnit.MILLISECONDS)
        .readTimeout(1200, TimeUnit.MILLISECONDS)
        .writeTimeout(1200, TimeUnit.MILLISECONDS)
        .build()
    private val jsonMediaType = "application/json".toMediaType()

    private val _state = MutableStateFlow(BadgeUsbState())
    val state: StateFlow<BadgeUsbState> = _state.asStateFlow()

    private var receiverRegistered = false
    private var readJob: Job? = null
    private var apPollJob: Job? = null
    private var activeConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var activeInterface: UsbInterface? = null
    private var activeOutEndpoint: UsbEndpoint? = null

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device = intent.usbDeviceExtra() ?: return
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        scope.launch { connectToDevice(device) }
                    } else {
                        setState {
                            it.copy(
                                status = BadgeUsbStatus.PERMISSION_NEEDED,
                                deviceName = device.displayName(),
                                message = "USB permission denied"
                            )
                        }
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> refresh()
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val detached = intent.usbDeviceExtra()
                    if (detached != null && detached.vendorId == ESPRESSIF_VENDOR_ID) {
                        disconnect("Badge disconnected")
                    }
                }
            }
        }
    }

    fun start() {
        registerReceiverIfNeeded()
        refresh()
        startApPoller()
    }

    fun stop() {
        disconnect("Badge USB stopped")
        apPollJob?.cancel()
        apPollJob = null
        if (receiverRegistered) {
            runCatching { context.unregisterReceiver(usbReceiver) }
            receiverRegistered = false
        }
    }

    fun refresh() {
        val device = findBadgeDevice()
        if (device == null) {
            setState {
                it.copy(
                    status = BadgeUsbStatus.DISCONNECTED,
                    deviceName = null,
                    message = "Connect USB-C or join the FoF badge AP"
                )
            }
            return
        }

        if (!usbManager.hasPermission(device)) {
            setState {
                it.copy(
                    status = BadgeUsbStatus.PERMISSION_NEEDED,
                    deviceName = device.displayName(),
                    message = "FoF badge found. Tap Connect to grant USB access."
                )
            }
            return
        }

        scope.launch { connectToDevice(device) }
    }

    fun requestConnection() {
        registerReceiverIfNeeded()
        val device = findBadgeDevice()
        if (device == null) {
            refresh()
            return
        }
        if (usbManager.hasPermission(device)) {
            scope.launch { connectToDevice(device) }
            return
        }

        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_IMMUTABLE else 0
        val permissionIntent = PendingIntent.getBroadcast(
            context,
            0,
            Intent(ACTION_USB_PERMISSION).setPackage(context.packageName),
            flags
        )
        usbManager.requestPermission(device, permissionIntent)
        setState {
            it.copy(
                status = BadgeUsbStatus.PERMISSION_NEEDED,
                deviceName = device.displayName(),
                message = "Waiting for USB permission"
            )
        }
    }

    fun sendPing() {
        scope.launch {
            writeLine("FOF_PING")
        }
    }

    fun requestStatus() {
        scope.launch {
            if (hasUsbCommandPath()) {
                writeLine("FOF_STATUS")
            } else {
                fetchApStatus(showErrors = true)
            }
        }
    }

    fun setMode(mode: String) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "set_mode")
            addProperty("mode", mode)
            addProperty("persist", true)
        })
    }

    fun rebootBadge() {
        sendControl(JsonObject().apply {
            addProperty("cmd", "reboot")
        })
    }

    fun enterBootloader() {
        sendControl(JsonObject().apply {
            addProperty("cmd", "bootloader")
        })
    }

    fun relayScannerFirmware(uart: String, force: Boolean = false) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "fw_relay")
            addProperty("uart", uart)
            addProperty("force", force)
        })
    }

    fun applyDisplayPolicy(policy: BadgeDisplayPolicy, persist: Boolean = true) {
        sendControl(badgeDisplayPolicyCommandJson(policy, persist))
    }

    fun resetDisplayPolicy(persist: Boolean = true) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "badge_display_policy_reset")
            addProperty("persist", persist)
        })
    }

    fun flashScannerFirmware(
        uart: String,
        name: String,
        version: String,
        firmware: ByteArray,
        forceRelay: Boolean = false
    ) {
        scope.launch {
            if (!hasUsbCommandPath()) {
                setState { it.copy(message = "USB command path required for scanner flashing") }
                return@launch
            }
            val crc = CRC32().apply { update(firmware) }.value
            setState {
                it.copy(
                    message = "Uploading scanner firmware to badge",
                    firmwareProgress = BadgeFirmwareProgress(
                        kind = "upload",
                        stage = "begin",
                        total = firmware.size.toLong()
                    )
                )
            }
            writeLine("FOF_CTL:${JsonObject().apply {
                addProperty("cmd", "fw_upload_begin")
                addProperty("name", name)
                addProperty("version", version)
                addProperty("size", firmware.size)
                addProperty("crc32", crc)
            }}")
            writeBytes(firmware)
            delay(500)
            writeLine("FOF_CTL:${JsonObject().apply {
                addProperty("cmd", "fw_relay")
                addProperty("uart", uart)
                addProperty("force", forceRelay)
            }}")
        }
    }

    private fun sendControl(payload: JsonObject) {
        scope.launch {
            if (hasUsbCommandPath()) {
                writeLine("FOF_CTL:$payload")
            } else {
                postApControl(payload)
            }
        }
    }

    private fun startApPoller() {
        if (apPollJob?.isActive == true) return
        apPollJob = scope.launch {
            while (isActive) {
                if (!hasUsbCommandPath()) {
                    fetchApStatus(showErrors = false)
                }
                delay(AP_POLL_INTERVAL_MS)
            }
        }
    }

    private fun registerReceiverIfNeeded() {
        if (receiverRegistered) return

        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            context.registerReceiver(usbReceiver, filter)
        }
        receiverRegistered = true
    }

    private fun findBadgeDevice(): UsbDevice? {
        return usbManager.deviceList.values.firstOrNull { device ->
            device.vendorId == ESPRESSIF_VENDOR_ID ||
                device.safeManufacturerName().orEmpty().contains("Espressif", ignoreCase = true) ||
                device.safeProductName().orEmpty().contains("JTAG", ignoreCase = true)
        }
    }

    private suspend fun connectToDevice(device: UsbDevice) {
        connectionMutex.withLock {
            val existingDevice = state.value.deviceName
            if (state.value.status == BadgeUsbStatus.CONNECTED &&
                existingDevice == device.displayName()
            ) {
                return
            }

            disconnectLocked()
            setState {
                it.copy(
                    status = BadgeUsbStatus.CONNECTING,
                    deviceName = device.displayName(),
                    message = "Opening badge USB serial"
                )
            }

            val port = findReadablePort(device)
            if (port == null) {
                setState {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "No readable USB serial endpoint found"
                    )
                }
                return
            }

            val connection = usbManager.openDevice(device)
            if (connection == null) {
                setState {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "Could not open USB badge"
                    )
                }
                return
            }

            if (!connection.claimInterface(port.usbInterface, true)) {
                connection.close()
                setState {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "Could not claim USB badge interface"
                    )
                }
                return
            }

            activeConnection = connection
            activeInterface = port.usbInterface
            activeOutEndpoint = port.outEndpoint
            setState {
                it.copy(
                    status = BadgeUsbStatus.CONNECTED,
                    deviceName = device.displayName(),
                    message = "Badge USB connected"
                )
            }
            writeLine("FOF_PING")
            writeLine("FOF_STATUS")
            startReader(connection, port.inEndpoint, device.displayName())
        }
    }

    private fun startReader(
        connection: android.hardware.usb.UsbDeviceConnection,
        inEndpoint: UsbEndpoint,
        deviceName: String
    ) {
        readJob?.cancel()
        readJob = scope.launch {
            val buffer = ByteArray(256)
            val lineBuffer = StringBuilder()
            try {
                while (isActive) {
                    val read = connection.bulkTransfer(
                        inEndpoint,
                        buffer,
                        buffer.size,
                        READ_TIMEOUT_MS
                    )
                    if (read > 0) {
                        for (i in 0 until read) {
                            val ch = buffer[i].toInt().toChar()
                            if (ch == '\n' || ch == '\r') {
                                if (lineBuffer.isNotEmpty()) {
                                    handleLine(lineBuffer.toString())
                                    lineBuffer.clear()
                                }
                            } else if (lineBuffer.length < MAX_LINE_CHARS) {
                                lineBuffer.append(ch)
                            } else {
                                Log.w(TAG, "Dropping overlong badge line")
                                lineBuffer.clear()
                            }
                        }
                    } else {
                        delay(25)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Badge USB reader stopped", e)
                setState {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = deviceName,
                        message = "Badge USB read failed: ${e.message ?: "unknown error"}"
                    )
                }
            }
        }
    }

    private suspend fun writeLine(line: String) = withContext(Dispatchers.IO) {
        val connection = activeConnection ?: return@withContext
        val out = activeOutEndpoint ?: return@withContext
        val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
        connection.bulkTransfer(out, bytes, bytes.size, WRITE_TIMEOUT_MS)
    }

    private suspend fun writeBytes(bytes: ByteArray) = withContext(Dispatchers.IO) {
        val connection = activeConnection ?: return@withContext
        val out = activeOutEndpoint ?: return@withContext
        var offset = 0
        while (offset < bytes.size) {
            val len = minOf(FW_CHUNK_BYTES, bytes.size - offset)
            val written = connection.bulkTransfer(out, bytes, offset, len, WRITE_TIMEOUT_MS)
            if (written <= 0) {
                setState { it.copy(message = "Firmware upload stalled at $offset/${bytes.size}") }
                return@withContext
            }
            offset += written
            setState {
                it.copy(
                    firmwareProgress = BadgeFirmwareProgress(
                        kind = "upload",
                        stage = "bytes",
                        percent = ((offset.toLong() * 100L) / bytes.size.coerceAtLeast(1)).toInt(),
                        bytes = offset.toLong(),
                        total = bytes.size.toLong()
                    )
                )
            }
        }
    }

    private fun hasUsbCommandPath(): Boolean {
        return state.value.status == BadgeUsbStatus.CONNECTED &&
            activeConnection != null &&
            activeOutEndpoint != null
    }

    private suspend fun fetchApStatus(showErrors: Boolean): Boolean = withContext(Dispatchers.IO) {
        val request = Request.Builder()
            .url("$BADGE_AP_BASE_URL/api/badge/status")
            .get()
            .build()

        val result = runCatching {
            badgeHttpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) return@use null
                parseStatus(response.body?.string().orEmpty())
            }
        }
        val status = result.getOrNull()
        if (status != null) {
            setState { current ->
                if (current.status == BadgeUsbStatus.CONNECTED) {
                    current.copy(controlStatus = status)
                } else {
                    current.copy(
                        status = BadgeUsbStatus.AP_CONNECTED,
                        deviceName = "FoF Badge AP",
                        message = "Badge AP connected",
                        controlStatus = status
                    )
                }
            }
            true
        } else {
            if (showErrors) {
                setState { current ->
                    current.copy(message = "Badge AP not reachable at 192.168.4.1")
                }
            }
            false
        }
    }

    private suspend fun postApControl(payload: JsonObject) = withContext(Dispatchers.IO) {
        val request = Request.Builder()
            .url("$BADGE_AP_BASE_URL/api/badge/control")
            .post(payload.toString().toRequestBody(jsonMediaType))
            .build()

        val ok = runCatching {
            badgeHttpClient.newCall(request).execute().use { response ->
                response.isSuccessful
            }
        }.getOrDefault(false)

        if (ok) {
            setState { it.copy(message = "Badge AP command accepted") }
            fetchApStatus(showErrors = false)
        } else {
            setState { it.copy(message = "Badge AP command failed") }
        }
    }

    private fun handleLine(line: String) {
        val trimmed = line.trim()
        if (trimmed.isEmpty()) return

        val detection = if (trimmed.startsWith("FOF_DET:")) {
            parseDetection(trimmed.removePrefix("FOF_DET:"))
        } else {
            null
        }
        val status = if (trimmed.startsWith("FOF_STATUS:")) {
            parseStatus(trimmed.removePrefix("FOF_STATUS:"))
        } else {
            null
        }
        val firmwareProgress = when {
            trimmed.startsWith("FOF_FW_UPLOAD:") ->
                parseFirmwareProgress("upload", trimmed.removePrefix("FOF_FW_UPLOAD:"))
            trimmed.startsWith("FOF_FW_RELAY_PROGRESS:") ->
                parseFirmwareProgress("relay", trimmed.removePrefix("FOF_FW_RELAY_PROGRESS:"))
            trimmed.startsWith("FOF_FW_RELAY:") ->
                parseFirmwareProgress("relay", trimmed.removePrefix("FOF_FW_RELAY:"))
            else -> null
        }

        setState { current ->
            val nextDetections = detection?.let {
                (listOf(it) + current.detections).take(MAX_RECENT_DETECTIONS)
            } ?: current.detections

            current.copy(
                lastLine = trimmed.take(160),
                eventCount = if (detection != null) current.eventCount + 1 else current.eventCount,
                detections = nextDetections,
                controlStatus = status ?: current.controlStatus,
                firmwareProgress = firmwareProgress ?: current.firmwareProgress,
                message = when {
                    trimmed.startsWith("FOF_PONG:") -> "Badge replied ${trimmed.removePrefix("FOF_PONG:")}"
                    status != null -> "Badge status updated"
                    firmwareProgress != null -> firmwareProgress.error.ifBlank {
                        "Firmware ${firmwareProgress.kind} ${firmwareProgress.stage} ${firmwareProgress.percent}%"
                    }
                    trimmed.startsWith("FOF_CTL_OK:") -> "Badge command accepted"
                    trimmed.startsWith("FOF_CTL_ERROR:") -> "Badge command failed"
                    detection != null -> "Receiving badge events"
                    else -> current.message
                }
            )
        }
    }

    private fun parseDetection(json: String): BadgeUsbDetection? {
        return runCatching {
            val obj = JsonParser.parseString(json).asJsonObject
            BadgeUsbDetection(
                id = obj.get("id")?.asString.orEmpty(),
                manufacturer = obj.get("manufacturer")?.asString.orEmpty(),
                source = obj.get("source")?.asInt ?: -1,
                confidence = obj.get("confidence")?.asFloat ?: 0f,
                rssi = obj.get("rssi")?.asInt ?: 0
            )
        }.getOrNull()
    }

    private fun parseFirmwareProgress(kind: String, json: String): BadgeFirmwareProgress? {
        return runCatching {
            val obj = JsonParser.parseString(json).asJsonObject
            val total = obj.optLong("total")
                .takeIf { it > 0 }
                ?: obj.optLong("size").takeIf { it > 0 }
                ?: obj.optLong("ota_total").takeIf { it > 0 }
                ?: 0L
            val bytes = obj.optLong("bytes")
                .takeIf { it > 0 }
                ?: obj.optLong("received").takeIf { it > 0 }
                ?: obj.optLong("ota_received").takeIf { it > 0 }
                ?: 0L
            BadgeFirmwareProgress(
                kind = kind,
                ok = obj.get("ok")?.asBoolean,
                uart = obj.optString("uart").ifBlank { obj.optString("slot") },
                stage = obj.optString("stage").ifBlank {
                    if (obj.optBoolean("ok")) "done" else "progress"
                },
                percent = obj.optInt("percent").takeIf { it > 0 }
                    ?: if (total > 0) ((bytes * 100L) / total).toInt() else 0,
                bytes = bytes,
                total = total,
                error = obj.optString("error")
            )
        }.getOrNull()
    }

    private fun parseDisplayPolicy(obj: JsonObject?): BadgeDisplayPolicy {
        if (obj == null) return defaultBadgeDisplayPolicy()
        val classesObj = runCatching { obj.getAsJsonObject("classes") }.getOrNull()
            ?: return defaultBadgeDisplayPolicy()
        val defaults = defaultBadgeDisplayPolicyClasses()
        val parsed = defaults.toMutableMap()
        BadgeDisplayPolicyClasses.forEach { info ->
            val classObj = runCatching { classesObj.getAsJsonObject(info.key) }.getOrNull()
            if (classObj != null) {
                val fallback = defaults.getValue(info.key)
                parsed[info.key] = BadgeDisplayClassPolicy(
                    enabled = classObj.optBoolean("enabled", fallback.enabled),
                    lane = classObj.optString("lane").ifBlank { fallback.lane },
                    minProximity = classObj.optString("min_proximity")
                        .ifBlank { fallback.minProximity },
                    priority = classObj.optInt("priority", fallback.priority).coerceIn(0, 100)
                )
            }
        }
        return BadgeDisplayPolicy(
            version = obj.optInt("version", 1),
            classes = parsed
        )
    }

    private fun parseIntMap(obj: JsonObject?): Map<String, Int> {
        if (obj == null) return emptyMap()
        return obj.entrySet().associate { (key, value) ->
            key to runCatching { value.asInt }.getOrDefault(0)
        }
    }

    private fun parseStatus(json: String): BadgeControlStatus? {
        return runCatching {
            val obj = JsonParser.parseString(json).asJsonObject
            val countsObj = obj.getAsJsonObject("counts")
            val reportingObj = obj.getAsJsonObject("reporting")
            val displayPolicy = parseDisplayPolicy(
                runCatching { obj.getAsJsonObject("display_policy") }.getOrNull()
            )
            val filteredCounts = parseIntMap(
                runCatching { obj.getAsJsonObject("filtered_counts") }.getOrNull()
            )
            val entities = obj.getAsJsonArray("entities")?.mapNotNull { element ->
                runCatching {
                    val e = element.asJsonObject
                    BadgeThreatEntity(
                        label = e.optString("label"),
                        detail = e.optString("detail"),
                        threatClass = e.optString("class"),
                        category = e.optString("category"),
                        code = e.optString("code"),
                        displayId = e.optString("display_id"),
                        score = e.optInt("score"),
                        evidenceQuality = e.optInt("evidence_quality"),
                        displayRank = e.optInt("display_rank"),
                        ageSeconds = e.optInt("age_s"),
                        lastSeenSeconds = e.optInt("last_seen_s"),
                        rssi = e.optInt("rssi"),
                        bestRssi = e.optInt("best_rssi"),
                        events = e.optInt("events"),
                        seenCount = e.optInt("seen_count"),
                        groupCount = e.optInt("group_count"),
                        proximityLevel = e.optInt("proximity_level"),
                        stale = e.optBoolean("stale"),
                        lat = e.optDoubleOrNull("lat"),
                        lon = e.optDoubleOrNull("lon"),
                        altitudeM = e.optFloatOrNull("altitude_m"),
                        operatorLat = e.optDoubleOrNull("operator_lat"),
                        operatorLon = e.optDoubleOrNull("operator_lon"),
                        operatorId = e.optString("operator_id").ifBlank { null }
                    )
                }.getOrNull()
            }.orEmpty()
            val scanners = obj.getAsJsonArray("scanners")?.mapNotNull { element ->
                runCatching {
                    val s = element.asJsonObject
                    BadgeScannerStatus(
                        slot = s.optInt("slot", -1),
                        uart = s.optString("uart"),
                        connected = s.optBoolean("connected"),
                        slotRole = s.optString("slot_role"),
                        expectedScanProfile = s.optString("expected_scan_profile"),
                        scanProfile = s.optString("scan_profile"),
                        roleAcked = s.optBoolean("role_acked"),
                        health = s.optString("health"),
                        uartRawSeen = s.optBoolean("uart_raw_seen"),
                        uartRawAgeSeconds = s.optLongOrNull("uart_raw_age_s"),
                        uartJsonErrors = s.optInt("uart_json_err"),
                        commandRx = s.optInt("cmd_rx"),
                        commandLastAgeSeconds = s.optLongOrNull("cmd_last_age_s"),
                        bleAdvSeen = s.optInt("ble_adv_seen"),
                        bleFpEmit = s.optInt("ble_fp_emit"),
                        bleMetaSeen = s.optInt("ble_meta_seen"),
                        bleTrackerSeen = s.optInt("ble_tracker_seen"),
                        ridEmit = s.optInt("rid_emit"),
                        privacySeen = s.optInt("privacy_seen"),
                        wifiTotalFrames = s.optInt("wifi_total_frames"),
                        wifiDroneSsidEmit = s.optInt("wifi_drone_ssid_emit"),
                        wifiNotableSsidEmit = s.optInt("wifi_notable_ssid_emit"),
                        wifiLastDroneSsid = s.optString("wifi_last_drone_ssid"),
                        wifiLastNotableSsid = s.optString("wifi_last_notable_ssid"),
                        displayPolicyHash = s.optLong("display_policy_hash"),
                        displayPolicyAckHash = s.optLong("display_policy_ack_hash"),
                        filteredCounts = parseIntMap(
                            runCatching { s.getAsJsonObject("filtered_counts") }.getOrNull()
                        ),
                        firmwareState = s.optString("fw_state"),
                        targetVersion = s.optString("target_ver"),
                        otaState = s.optString("ota_state"),
                        lastFirmwareError = s.optString("last_fw_error")
                    )
                }.getOrNull()
            }.orEmpty()

            BadgeControlStatus(
                version = obj.optString("version"),
                mode = obj.optString("mode").ifBlank { "local_ap" },
                modeLabel = obj.optString("mode_label").ifBlank { "Local AP" },
                threatScore = obj.optFloat("threat_score"),
                colorRgb565 = obj.optInt("color_rgb565"),
                reporting = BadgeReportingStatus(
                    networkMode = reportingObj?.optString("network_mode")
                        ?: obj.optString("network_mode").ifBlank { obj.optString("mode") },
                    backendEnabled = reportingObj?.optBoolean("backend_enabled")
                        ?: obj.optBoolean("backend_enabled"),
                    networkTtlSeconds = reportingObj?.optInt("network_ttl_s")
                        ?: obj.optInt("network_ttl_s"),
                    wifiSta = reportingObj?.optBoolean("wifi_sta") ?: obj.optBoolean("wifi_sta"),
                    standalone = reportingObj?.optBoolean("standalone") ?: false,
                    uploadsOk = reportingObj?.optInt("uploads_ok") ?: 0,
                    uploadsFail = reportingObj?.optInt("uploads_fail") ?: 0,
                    lastUploadAgeSeconds = reportingObj?.optLongOrNull("last_upload_age_s")
                ),
                counts = BadgeThreatCounts(
                    drone = countsObj?.get("drone")?.asInt ?: 0,
                    meta = countsObj?.get("meta")?.asInt ?: 0,
                    tracker = countsObj?.get("tracker")?.asInt ?: 0,
                    wifiAnomaly = countsObj?.get("wifi_anomaly")?.asInt ?: 0,
                    ble = countsObj?.get("ble")?.asInt ?: 0,
                    other = countsObj?.get("other")?.asInt ?: 0
                ),
                entities = entities,
                scanners = scanners,
                displayPolicy = displayPolicy,
                displayPolicyHash = obj.optLong("display_policy_hash"),
                filteredCounts = filteredCounts
            )
        }.getOrNull()
    }

    private fun findReadablePort(device: UsbDevice): UsbPort? {
        for (i in 0 until device.interfaceCount) {
            val usbInterface = device.getInterface(i)
            var inEndpoint: UsbEndpoint? = null
            var outEndpoint: UsbEndpoint? = null
            for (e in 0 until usbInterface.endpointCount) {
                val endpoint = usbInterface.getEndpoint(e)
                if (endpoint.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (endpoint.direction == UsbConstants.USB_DIR_IN) {
                    inEndpoint = endpoint
                } else if (endpoint.direction == UsbConstants.USB_DIR_OUT) {
                    outEndpoint = endpoint
                }
            }
            if (inEndpoint != null && outEndpoint != null) {
                return UsbPort(usbInterface, inEndpoint, outEndpoint)
            }
        }
        return null
    }

    private fun disconnect(reason: String) {
        scope.launch {
            connectionMutex.withLock {
                disconnectLocked()
                setState {
                    it.copy(
                        status = BadgeUsbStatus.DISCONNECTED,
                        deviceName = null,
                        message = reason
                    )
                }
            }
        }
    }

    private fun disconnectLocked() {
        readJob?.cancel()
        readJob = null
        val connection = activeConnection
        val usbInterface = activeInterface
        if (connection != null && usbInterface != null) {
            runCatching { connection.releaseInterface(usbInterface) }
        }
        runCatching { connection?.close() }
        activeConnection = null
        activeInterface = null
        activeOutEndpoint = null
    }

    private fun setState(update: (BadgeUsbState) -> BadgeUsbState) {
        _state.value = update(_state.value)
    }

    private fun UsbDevice.displayName(): String {
        val parts = listOfNotNull(
            safeManufacturerName()?.takeIf { it.isNotBlank() },
            safeProductName()?.takeIf { it.isNotBlank() }
        )
        return parts.joinToString(" ").ifBlank { deviceName }
    }

    private fun UsbDevice.safeManufacturerName(): String? {
        return runCatching { manufacturerName }.getOrNull()
    }

    private fun UsbDevice.safeProductName(): String? {
        return runCatching { productName }.getOrNull()
    }

    private fun Intent.usbDeviceExtra(): UsbDevice? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
    }

    private data class UsbPort(
        val usbInterface: UsbInterface,
        val inEndpoint: UsbEndpoint,
        val outEndpoint: UsbEndpoint
    )

    private fun JsonObject.optString(key: String): String {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asString.orEmpty() }
            .getOrDefault("")
    }

    private fun JsonObject.optInt(key: String, fallback: Int = 0): Int {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asInt ?: fallback }
            .getOrDefault(fallback)
    }

    private fun JsonObject.optLong(key: String, fallback: Long = 0L): Long {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong ?: fallback }
            .getOrDefault(fallback)
    }

    private fun JsonObject.optLongOrNull(key: String): Long? {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong }.getOrNull()
    }

    private fun JsonObject.optFloat(key: String, fallback: Float = 0f): Float {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat ?: fallback }
            .getOrDefault(fallback)
    }

    private fun JsonObject.optFloatOrNull(key: String): Float? {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat }.getOrNull()
    }

    private fun JsonObject.optDoubleOrNull(key: String): Double? {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asDouble }.getOrNull()
    }

    private fun JsonObject.optBoolean(key: String, fallback: Boolean = false): Boolean {
        return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asBoolean ?: fallback }
            .getOrDefault(fallback)
    }
}
