package com.friendorfoe.data.badge

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import com.friendorfoe.BuildConfig
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
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
import java.net.URLEncoder
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.zip.CRC32

enum class BadgeUsbStatus {
    DISCONNECTED,
    PERMISSION_NEEDED,
    CONNECTING,
    AP_CONNECTED,
    DEBUG_BRIDGE_CONNECTED,
    BLE_CONNECTED,
    CONNECTED,
    ERROR
}

data class BadgeUsbDetection(
    val id: String,
    val manufacturer: String,
    val badgeLabel: String = "",
    val badgeClass: String = "",
    val badgeEntityKey: String = "",
    val source: Int,
    val confidence: Float,
    val threatScore: Float = 0f,
    val rssi: Int
)

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
        private const val DEBUG_BRIDGE_BASE_URL = "http://10.0.2.2:8765"
        private const val READ_TIMEOUT_MS = 250
        private const val WRITE_TIMEOUT_MS = 250
        private const val AP_POLL_INTERVAL_MS = 2500L
        private const val DEBUG_BRIDGE_POLL_INTERVAL_MS = 1500L
        private const val BLE_SCAN_INTERVAL_MS = 6000L
        private const val BLE_SCAN_WINDOW_MS = 4500L
        private const val USB_STATUS_POLL_INTERVAL_MS = 2000L
        private const val MAX_RECENT_DETECTIONS = 20
        private const val MAX_LINE_CHARS = 8192
        private const val FW_CHUNK_BYTES = 1024
        private val BADGE_BLE_SERVICE_UUID: UUID =
            UUID.fromString("0000f0f0-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_STATUS_UUID: UUID =
            UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_CONTROL_UUID: UUID =
            UUID.fromString("0000ff02-0000-1000-8000-00805f9b34fb")
        private val CLIENT_CONFIG_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val connectionMutex = Mutex()
    private val recoveryTracker = BadgeRecoveryTracker()
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
    private var debugBridgePollJob: Job? = null
    private var blePollJob: Job? = null
    private var usbStatusPollJob: Job? = null
    private var activeConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var activeInterface: UsbInterface? = null
    private var activeOutEndpoint: UsbEndpoint? = null
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager?.adapter
    @Volatile private var activeGatt: BluetoothGatt? = null
    @Volatile private var activeBleControlChar: BluetoothGattCharacteristic? = null
    @Volatile private var activeBleStatusChar: BluetoothGattCharacteristic? = null
    @Volatile private var bleScanning = false

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
                                message = "USB permission denied",
                                transportLabel = "USB-C"
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
        startBlePoller()
        startApPoller()
        startDebugBridgePoller()
    }

    fun stop() {
        disconnect("Badge USB stopped")
        apPollJob?.cancel()
        apPollJob = null
        debugBridgePollJob?.cancel()
        debugBridgePollJob = null
        blePollJob?.cancel()
        blePollJob = null
        closeBle("Badge BLE stopped")
        if (receiverRegistered) {
            runCatching { context.unregisterReceiver(usbReceiver) }
            receiverRegistered = false
        }
    }

    fun refresh() {
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            if (hasBleCommandPath()) {
                return
            }
            setState {
                it.copy(
                    status = BadgeUsbStatus.DISCONNECTED,
                    deviceName = null,
                    message = "Connect USB-C or join the FoF badge AP",
                    transportLabel = ""
                )
            }
            return
        }
        if (candidates.size > 1) {
            reportAmbiguousBadgeDevices(candidates)
            return
        }
        val device = candidates.first()

        if (!usbManager.hasPermission(device)) {
            setState {
                it.copy(
                    status = BadgeUsbStatus.PERMISSION_NEEDED,
                    deviceName = device.displayName(),
                    message = "FoF badge found. Tap Connect to grant USB access.",
                    transportLabel = "USB-C"
                )
            }
            return
        }

        scope.launch { connectToDevice(device) }
    }

    fun requestConnection() {
        registerReceiverIfNeeded()
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            refresh()
            return
        }
        if (candidates.size > 1) {
            reportAmbiguousBadgeDevices(candidates)
            return
        }
        val device = candidates.first()
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
                message = "Waiting for USB permission",
                transportLabel = "USB-C"
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
            } else if (hasBleCommandPath()) {
                readBleStatus()
            } else {
                fetchNetworkStatus(showErrors = true)
            }
        }
    }

    fun setMode(mode: BadgeNetworkMode) {
        sendControl(badgeNetworkModeCommandJson(mode))
    }

    fun rebootBadge() {
        sendRecoveryControl(BadgeRecoveryCommand.REBOOT, badgeRebootCommandJson())
    }

    fun enterBootloader() {
        sendRecoveryControl(BadgeRecoveryCommand.BOOTLOADER, badgeBootloaderCommandJson())
    }

    fun relayScannerFirmware(uart: String, force: Boolean = false) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "fw_relay")
            addProperty("uart", uart)
            addProperty("force", force)
        })
    }

    fun applyDisplayPolicy(policy: BadgeDisplayPolicy) {
        sendControl(badgeDisplayPolicyCommandJson(policy))
    }

    fun resetDisplayPolicy(persist: Boolean = true) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "badge_display_policy_reset")
            addProperty("persist", persist)
        })
    }

    fun applyBadgeTheme(theme: BadgeTheme) {
        sendControl(badgeThemeCommandJson(theme))
    }

    fun resetBadgeTheme(persist: Boolean = true) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "badge_theme_reset")
            addProperty("persist", persist)
        })
    }

    fun displayNav(action: BadgeDisplayAction) {
        sendControl(badgeDisplayNavCommandJson(action))
    }

    fun flashScannerFirmware(
        uart: String,
        name: String,
        version: String,
        firmware: ByteArray,
        forceRelay: Boolean = false
    ) {
        scope.launch {
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
            val httpBase = activeHttpBaseUrl()
            if (!hasUsbCommandPath() && httpBase != null) {
                uploadScannerFirmwareOverHttp(httpBase, uart, name, version, firmware, forceRelay)
                return@launch
            }
            if (!hasUsbCommandPath()) {
                setState { it.copy(message = "USB, Badge AP, or Debug Bridge required for scanner flashing") }
                return@launch
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
            } else if (hasBleCommandPath()) {
                writeBleControl(payload)
            } else {
                postNetworkControl(payload)
            }
        }
    }

    private fun sendRecoveryControl(
        command: BadgeRecoveryCommand,
        payload: JsonObject
    ) {
        scope.launch {
            if (!isDirectUsbRecoverySupported(state.value.status) || !hasUsbCommandPath()) {
                setState {
                    it.copy(message = "Recovery commands require a direct USB-C connection")
                }
                return@launch
            }
            if (!recoveryTracker.begin(command)) {
                setState { it.copy(message = "A badge recovery command is already pending") }
                return@launch
            }
            try {
                writeLine("FOF_CTL:$payload")
                setState {
                    it.copy(
                        message = when (command) {
                            BadgeRecoveryCommand.REBOOT -> "Waiting for badge reboot acknowledgement"
                            BadgeRecoveryCommand.BOOTLOADER -> {
                                "Waiting for badge bootloader acknowledgement"
                            }
                        }
                    )
                }
            } catch (cancelled: CancellationException) {
                recoveryTracker.cancel(command)
                throw cancelled
            } catch (error: Exception) {
                recoveryTracker.cancel(command)
                setState {
                    it.copy(message = "Badge recovery command failed: ${error.message.orEmpty()}")
                }
            }
        }
    }

    private fun startApPoller() {
        if (apPollJob?.isActive == true) return
        apPollJob = scope.launch {
            while (isActive) {
                if (!hasUsbCommandPath() && !hasBleCommandPath()) {
                    fetchApStatus(showErrors = false)
                }
                delay(AP_POLL_INTERVAL_MS)
            }
        }
    }

    private fun startBlePoller() {
        if (blePollJob?.isActive == true) return
        blePollJob = scope.launch {
            while (isActive) {
                if (!hasUsbCommandPath() && !hasBleCommandPath() &&
                    state.value.status != BadgeUsbStatus.AP_CONNECTED
                ) {
                    startBleScanIfPossible()
                } else if (hasBleCommandPath()) {
                    readBleStatus()
                }
                delay(BLE_SCAN_INTERVAL_MS)
            }
        }
    }

    private fun startDebugBridgePoller() {
        if (!BuildConfig.DEBUG || debugBridgePollJob?.isActive == true) return
        debugBridgePollJob = scope.launch {
            while (isActive) {
                if (!hasUsbCommandPath() && !hasBleCommandPath() &&
                    state.value.status != BadgeUsbStatus.AP_CONNECTED
                ) {
                    fetchDebugBridgeStatus(showErrors = false)
                }
                delay(DEBUG_BRIDGE_POLL_INTERVAL_MS)
            }
        }
    }

    private fun startUsbStatusPoller() {
        if (usbStatusPollJob?.isActive == true) return
        usbStatusPollJob = scope.launch {
            var pollsWithoutStatus = 0
            while (isActive && hasUsbCommandPath()) {
                writeLine("FOF_STATUS")
                delay(USB_STATUS_POLL_INTERVAL_MS)
                if (state.value.controlStatus == null) {
                    pollsWithoutStatus++
                    if (pollsWithoutStatus >= 3) {
                        setState {
                            it.copy(
                                message = "USB serial open, waiting for badge app" +
                                    (it.deviceName?.let { name -> " ($name)" } ?: "")
                            )
                        }
                    }
                } else {
                    pollsWithoutStatus = 0
                }
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

    private fun findBadgeCandidates(): List<UsbDevice> {
        return usbManager.deviceList.values.filter { device ->
            device.vendorId == ESPRESSIF_VENDOR_ID ||
                device.safeManufacturerName().orEmpty().contains("Espressif", ignoreCase = true) ||
                device.safeProductName().orEmpty().contains("JTAG", ignoreCase = true)
        }
    }

    private fun reportAmbiguousBadgeDevices(candidates: List<UsbDevice>) {
        val names = candidates.take(3).joinToString(", ") { it.displayName() }
        setState {
            it.copy(
                status = BadgeUsbStatus.ERROR,
                deviceName = null,
                message = "Multiple Espressif USB devices found: $names. Connect only the badge for USB-C, or use Badge AP fallback.",
                transportLabel = "USB-C"
            )
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
                    message = "Opening badge USB serial",
                    transportLabel = "USB-C",
                    controlStatus = null
                )
            }

            val port = findReadablePort(device)
            if (port == null) {
                setState {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "No readable USB serial endpoint found",
                        transportLabel = "USB-C"
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
                        message = "Could not open USB badge",
                        transportLabel = "USB-C"
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
                        message = "Could not claim USB badge interface",
                        transportLabel = "USB-C"
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
                    message = "Badge USB connected",
                    transportLabel = "USB-C"
                )
            }
            writeLine("FOF_PING")
            writeLine("FOF_STATUS")
            startReader(connection, port.inEndpoint, device.displayName())
            startUsbStatusPoller()
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

    private fun hasBleCommandPath(): Boolean {
        return state.value.status == BadgeUsbStatus.BLE_CONNECTED &&
            activeGatt != null &&
            activeBleControlChar != null
    }

    private fun activeHttpBaseUrl(): String? {
        return when (state.value.status) {
            BadgeUsbStatus.AP_CONNECTED -> BADGE_AP_BASE_URL
            BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED -> DEBUG_BRIDGE_BASE_URL
            else -> null
        }
    }

    private suspend fun fetchNetworkStatus(showErrors: Boolean): Boolean {
        if (hasBlePermissions() && !hasBleCommandPath()) {
            startBleScanIfPossible()
            delay(600)
        }
        if (hasBleCommandPath()) {
            readBleStatus()
            return true
        }
        if (fetchApStatus(showErrors = false)) {
            return true
        }
        if (BuildConfig.DEBUG && fetchDebugBridgeStatus(showErrors = false)) {
            return true
        }
        if (showErrors) {
            setState { current ->
                current.copy(message = "Badge BLE/AP/Debug Bridge not reachable")
            }
        }
        return false
    }

    private suspend fun fetchApStatus(showErrors: Boolean): Boolean {
        return fetchHttpStatus(
            baseUrl = BADGE_AP_BASE_URL,
            connectedStatus = BadgeUsbStatus.AP_CONNECTED,
            deviceName = "FoF Badge AP",
            transportLabel = "Badge AP",
            connectedMessage = "Badge AP connected",
            errorMessage = "Badge AP not reachable at 192.168.4.1",
            showErrors = showErrors
        )
    }

    private suspend fun fetchDebugBridgeStatus(showErrors: Boolean): Boolean {
        if (!BuildConfig.DEBUG) return false
        return fetchHttpStatus(
            baseUrl = DEBUG_BRIDGE_BASE_URL,
            connectedStatus = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            deviceName = "FoF Debug Bridge",
            transportLabel = "Debug Bridge",
            connectedMessage = "Debug Bridge connected",
            errorMessage = "Debug Bridge not reachable at 10.0.2.2:8765",
            showErrors = showErrors
        )
    }

    private suspend fun fetchHttpStatus(
        baseUrl: String,
        connectedStatus: BadgeUsbStatus,
        deviceName: String,
        transportLabel: String,
        connectedMessage: String,
        errorMessage: String,
        showErrors: Boolean
    ): Boolean = withContext(Dispatchers.IO) {
        val request = Request.Builder()
            .url("$baseUrl/api/badge/status")
            .get()
            .build()

        val result = runCatching {
            badgeHttpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) return@use null
                val body = response.body?.string().orEmpty()
                val receivedAtElapsedMs = SystemClock.elapsedRealtime()
                parseBadgeControlStatus(body, receivedAtElapsedMs)
            }
        }
        val status = result.getOrNull()
        if (status != null) {
            setState { current ->
                if (current.status == BadgeUsbStatus.CONNECTED) {
                    current.copy(controlStatus = status)
                } else {
                    current.copy(
                        status = connectedStatus,
                        deviceName = deviceName,
                        message = connectedMessage,
                        transportLabel = transportLabel,
                        controlStatus = status
                    )
                }
            }
            true
        } else {
            if (showErrors) {
                setState { current ->
                    current.copy(message = errorMessage)
                }
            }
            false
        }
    }

    private suspend fun postNetworkControl(payload: JsonObject) = withContext(Dispatchers.IO) {
        val baseUrl = activeHttpBaseUrl() ?: when {
            fetchNetworkStatus(showErrors = false) -> activeHttpBaseUrl()
            else -> null
        }
        if (baseUrl == null) {
            setState { it.copy(message = "No badge HTTP control path available") }
            return@withContext
        }
        val request = Request.Builder()
            .url("$baseUrl/api/badge/control")
            .post(payload.toString().toRequestBody(jsonMediaType))
            .build()

        val ok = runCatching {
            badgeHttpClient.newCall(request).execute().use { response ->
                response.isSuccessful
            }
        }.getOrDefault(false)

        if (ok) {
            setState { it.copy(message = "${it.transportLabel.ifBlank { "Badge HTTP" }} command accepted") }
            fetchHttpStatus(
                baseUrl = baseUrl,
                connectedStatus = if (baseUrl == DEBUG_BRIDGE_BASE_URL) {
                    BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED
                } else {
                    BadgeUsbStatus.AP_CONNECTED
                },
                deviceName = if (baseUrl == DEBUG_BRIDGE_BASE_URL) "FoF Debug Bridge" else "FoF Badge AP",
                transportLabel = if (baseUrl == DEBUG_BRIDGE_BASE_URL) "Debug Bridge" else "Badge AP",
                connectedMessage = if (baseUrl == DEBUG_BRIDGE_BASE_URL) {
                    "Debug Bridge connected"
                } else {
                    "Badge AP connected"
                },
                errorMessage = "Badge HTTP status refresh failed",
                showErrors = false
            )
        } else {
            setState { it.copy(message = "${it.transportLabel.ifBlank { "Badge HTTP" }} command failed") }
        }
    }

    private suspend fun uploadScannerFirmwareOverHttp(
        baseUrl: String,
        uart: String,
        name: String,
        version: String,
        firmware: ByteArray,
        forceRelay: Boolean
    ) = withContext(Dispatchers.IO) {
        val encName = URLEncoder.encode(name, Charsets.UTF_8.name())
        val encVersion = URLEncoder.encode(version, Charsets.UTF_8.name())
        val uploadRequest = Request.Builder()
            .url("$baseUrl/api/fw/upload?name=$encName&version=$encVersion")
            .post(firmware.toRequestBody("application/octet-stream".toMediaType()))
            .build()
        val uploaded = runCatching {
            badgeHttpClient.newCall(uploadRequest).execute().use { response ->
                if (!response.isSuccessful) return@use false
                parseFirmwareProgress("upload", response.body?.string().orEmpty())?.also { progress ->
                    setState { it.copy(firmwareProgress = progress) }
                }
                true
            }
        }.getOrDefault(false)
        if (!uploaded) {
            setState { it.copy(message = "HTTP firmware upload failed") }
            return@withContext
        }

        val relayPayload = JsonObject().apply {
            addProperty("uart", uart)
            addProperty("force", forceRelay)
        }
        val relayRequest = Request.Builder()
            .url("$baseUrl/api/fw/relay")
            .post(relayPayload.toString().toRequestBody(jsonMediaType))
            .build()
        val relayed = runCatching {
            badgeHttpClient.newCall(relayRequest).execute().use { response ->
                if (!response.isSuccessful) return@use false
                parseFirmwareProgress("relay", response.body?.string().orEmpty())?.also { progress ->
                    setState { it.copy(firmwareProgress = progress) }
                }
                true
            }
        }.getOrDefault(false)
        setState {
            it.copy(message = if (relayed) "Scanner firmware relay requested" else "Scanner firmware relay failed")
        }
        fetchHttpStatus(
            baseUrl = baseUrl,
            connectedStatus = if (baseUrl == DEBUG_BRIDGE_BASE_URL) {
                BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED
            } else {
                BadgeUsbStatus.AP_CONNECTED
            },
            deviceName = if (baseUrl == DEBUG_BRIDGE_BASE_URL) "FoF Debug Bridge" else "FoF Badge AP",
            transportLabel = if (baseUrl == DEBUG_BRIDGE_BASE_URL) "Debug Bridge" else "Badge AP",
            connectedMessage = if (baseUrl == DEBUG_BRIDGE_BASE_URL) {
                "Debug Bridge connected"
            } else {
                "Badge AP connected"
            },
            errorMessage = "Badge HTTP status refresh failed",
            showErrors = false
        )
    }

    private fun hasBlePermissions(): Boolean {
        val adapter = bluetoothAdapter ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED &&
                ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED
            granted && runCatching { adapter.isEnabled }.getOrDefault(false)
        } else {
            val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED ||
                ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_COARSE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED
            granted && runCatching { adapter.isEnabled }.getOrDefault(false)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startBleScanIfPossible() {
        if (!hasBlePermissions() || bleScanning || activeGatt != null) {
            if (!hasBlePermissions() && state.value.status == BadgeUsbStatus.DISCONNECTED) {
                setState {
                    it.copy(
                        message = "Grant Bluetooth permissions or use USB-C/AP",
                        transportLabel = "BLE"
                    )
                }
            }
            return
        }
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        bleScanning = true
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = runCatching { result.device.name }.getOrNull()
                    ?: result.scanRecord?.deviceName.orEmpty()
                val hasService = result.scanRecord
                    ?.serviceUuids
                    ?.any { it.uuid == BADGE_BLE_SERVICE_UUID } == true
                if (!hasService && !name.contains("FoF Badge", ignoreCase = true)) return
                runCatching { scanner.stopScan(this) }
                bleScanning = false
                connectBle(result.device)
            }

            override fun onScanFailed(errorCode: Int) {
                bleScanning = false
                setState {
                    it.copy(
                        status = if (it.status == BadgeUsbStatus.DISCONNECTED) BadgeUsbStatus.ERROR else it.status,
                        message = "Badge BLE scan failed: $errorCode",
                        transportLabel = "BLE"
                    )
                }
            }
        }
        val filters = listOf(
            ScanFilter.Builder()
                .setServiceUuid(ParcelUuid(BADGE_BLE_SERVICE_UUID))
                .build()
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        runCatching { scanner.startScan(filters, settings, callback) }
            .onSuccess {
                setState { current ->
                    if (current.status == BadgeUsbStatus.DISCONNECTED) {
                        current.copy(
                            status = BadgeUsbStatus.CONNECTING,
                            deviceName = "FoF Badge BLE",
                            message = "Scanning for badge BLE tether",
                            transportLabel = "BLE"
                        )
                    } else current
                }
                scope.launch {
                    delay(BLE_SCAN_WINDOW_MS)
                    if (bleScanning) {
                        runCatching { scanner.stopScan(callback) }
                        bleScanning = false
                    }
                }
            }
            .onFailure { error ->
                bleScanning = false
                setState { it.copy(message = "Badge BLE scan failed: ${error.message}") }
            }
    }

    @SuppressLint("MissingPermission")
    private fun connectBle(device: BluetoothDevice) {
        if (!hasBlePermissions()) return
        closeBle("Switching badge BLE device")
        setState {
            it.copy(
                status = BadgeUsbStatus.CONNECTING,
                deviceName = runCatching { device.name }.getOrNull() ?: device.address,
                message = "Connecting badge BLE",
                transportLabel = "BLE",
                controlStatus = null
            )
        }
        activeGatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, badgeGattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            @Suppress("DEPRECATION")
            device.connectGatt(context, false, badgeGattCallback)
        }
    }

    private val badgeGattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                closeBle("Badge BLE disconnected")
                return
            }
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                activeGatt = gatt
                runCatching { gatt.requestMtu(512) }
                runCatching { gatt.discoverServices() }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                closeBle("Badge BLE service discovery failed")
                return
            }
            val service: BluetoothGattService? = gatt.getService(BADGE_BLE_SERVICE_UUID)
            activeBleStatusChar = service?.getCharacteristic(BADGE_BLE_STATUS_UUID)
            activeBleControlChar = service?.getCharacteristic(BADGE_BLE_CONTROL_UUID)
            if (activeBleStatusChar == null || activeBleControlChar == null) {
                closeBle("Badge BLE service missing")
                return
            }
            setState {
                it.copy(
                    status = BadgeUsbStatus.BLE_CONNECTED,
                    deviceName = runCatching { gatt.device.name }.getOrNull()
                        ?: gatt.device.address,
                    message = "Badge BLE connected",
                    transportLabel = "BLE"
                )
            }
            enableBleStatusNotifications(gatt, activeBleStatusChar)
            readBleStatus()
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS &&
                characteristic.uuid == BADGE_BLE_STATUS_UUID
            ) {
                @Suppress("DEPRECATION")
                handleBleStatusBytes(characteristic.value ?: byteArrayOf())
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS &&
                characteristic.uuid == BADGE_BLE_STATUS_UUID
            ) {
                handleBleStatusBytes(value)
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                @Suppress("DEPRECATION")
                handleBleStatusBytes(characteristic.value ?: byteArrayOf())
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                handleBleStatusBytes(value)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableBleStatusNotifications(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic?
    ) {
        if (characteristic == null || !hasBlePermissions()) return
        runCatching { gatt.setCharacteristicNotification(characteristic, true) }
        val descriptor = characteristic.getDescriptor(CLIENT_CONFIG_UUID) ?: return
        @Suppress("DEPRECATION")
        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(
                descriptor,
                BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            )
        } else {
            @Suppress("DEPRECATION")
            gatt.writeDescriptor(descriptor)
        }
    }

    @SuppressLint("MissingPermission")
    private fun readBleStatus() {
        val gatt = activeGatt ?: return
        val characteristic = activeBleStatusChar ?: return
        if (!hasBlePermissions()) return
        runCatching { gatt.readCharacteristic(characteristic) }
    }

    @SuppressLint("MissingPermission")
    private fun writeBleControl(payload: JsonObject) {
        val gatt = activeGatt
        val characteristic = activeBleControlChar
        if (gatt == null || characteristic == null || !hasBlePermissions()) {
            setState { it.copy(message = "Badge BLE not connected") }
            return
        }
        val bytes = payload.toString().toByteArray(Charsets.UTF_8)
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(
                characteristic,
                bytes,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            characteristic.value = bytes
            characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(characteristic)
        }
        setState {
            it.copy(message = if (ok) "Badge BLE command sent" else "Badge BLE command failed")
        }
        if (ok) {
            scope.launch {
                delay(350)
                readBleStatus()
            }
        }
    }

    private fun handleBleStatusBytes(bytes: ByteArray) {
        val json = bytes.toString(Charsets.UTF_8).trim()
        if (json.isBlank()) return
        val receivedAtElapsedMs = SystemClock.elapsedRealtime()
        val status = parseBadgeControlStatus(json, receivedAtElapsedMs)
        setState { current ->
            current.copy(
                status = BadgeUsbStatus.BLE_CONNECTED,
                deviceName = current.deviceName ?: "FoF Badge BLE",
                message = if (status != null) "Badge BLE status updated" else current.message,
                transportLabel = "BLE",
                lastLine = json.take(160),
                controlStatus = status ?: current.controlStatus
            )
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeBle(reason: String) {
        val gatt = activeGatt
        activeGatt = null
        activeBleControlChar = null
        activeBleStatusChar = null
        runCatching {
            if (hasBlePermissions()) {
                gatt?.disconnect()
            }
        }
        runCatching { gatt?.close() }
        if (state.value.status == BadgeUsbStatus.BLE_CONNECTED ||
            (state.value.status == BadgeUsbStatus.CONNECTING &&
                state.value.transportLabel == "BLE")
        ) {
            setState {
                it.copy(
                    status = BadgeUsbStatus.DISCONNECTED,
                    message = reason,
                    transportLabel = ""
                )
            }
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
            val receivedAtElapsedMs = SystemClock.elapsedRealtime()
            parseBadgeControlStatus(
                trimmed.removePrefix("FOF_STATUS:"),
                receivedAtElapsedMs
            )
        } else {
            null
        }
        val recoveryAcknowledgement = recoveryTracker.accept(trimmed)
        val recoveryStillPending = recoveryTracker.pendingCommand != null
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
                    recoveryAcknowledgement == BadgeRecoveryAcknowledgement.REBOOT_OK -> {
                        "Badge reboot acknowledged; reconnect after it restarts"
                    }
                    recoveryAcknowledgement == BadgeRecoveryAcknowledgement.BOOTLOADER_OK -> {
                        "Badge bootloader acknowledged; reconnect when recovery is complete"
                    }
                    trimmed.startsWith("FOF_PONG:") -> "Badge replied ${trimmed.removePrefix("FOF_PONG:")}"
                    status != null -> "Badge status updated"
                    firmwareProgress != null -> firmwareProgress.error.ifBlank {
                        "Firmware ${firmwareProgress.kind} ${firmwareProgress.stage} ${firmwareProgress.percent}%"
                    }
                    recoveryStillPending && trimmed.startsWith("FOF_CTL_OK:") -> {
                        "Ignoring unrelated control acknowledgement; waiting for recovery acknowledgement"
                    }
                    recoveryStillPending && trimmed.startsWith("FOF_CTL_ERROR:") -> {
                        "Recovery command was not acknowledged; waiting for its exact result"
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
                badgeLabel = obj.optString("badge_label"),
                badgeClass = obj.optString("badge_class"),
                badgeEntityKey = obj.optString("badge_entity_key"),
                source = obj.get("source")?.asInt ?: -1,
                confidence = obj.get("confidence")?.asFloat ?: 0f,
                threatScore = obj.optFloat("threat_score"),
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
        recoveryTracker.clear()
        readJob?.cancel()
        readJob = null
        usbStatusPollJob?.cancel()
        usbStatusPollJob = null
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
