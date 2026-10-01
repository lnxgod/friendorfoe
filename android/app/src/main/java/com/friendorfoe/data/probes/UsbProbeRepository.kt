package com.friendorfoe.data.probes

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import androidx.core.content.ContextCompat
import com.friendorfoe.data.remote.ProbeActivityDto
import com.friendorfoe.data.time.MonotonicClock
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.UUID

interface UsbProbeSource {
    val state: StateFlow<UsbProbeState>
    fun start()
    fun stop()
    fun connect()
}

data class UsbProbeState(
    val connected: Boolean = false,
    val connecting: Boolean = false,
    val message: String = "Plug in an ESP32-S3 with Wi-Fi Probe firmware using a USB data cable.",
    val snapshot: ProbeActivityDto = ProbeActivityDto(),
    val receivedMs: Long? = null,
    val dropped: Long = 0,
)

/** Owns only the dedicated TinyUSB probe product, never a badge or its ROM loader. */
@Singleton
class UsbProbeRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val clock: MonotonicClock,
) : UsbProbeSource {
    private val manager = context.getSystemService(UsbManager::class.java)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(UsbProbeState())
    override val state: StateFlow<UsbProbeState> = _state
    private val connectionMutex = Mutex()
    private var active = false
    private var generation = 0
    private var reader: Job? = null
    private var selectedName: String? = null
    private var pendingName: String? = null
    private var permissionIntent: PendingIntent? = null
    private var permissionToken: String? = null
    private val permissionAction = "${context.packageName}.PROBE_USB_PERMISSION"
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (!active) return
            @Suppress("DEPRECATION") val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> if (selectedName == null) connect()
                UsbManager.ACTION_USB_DEVICE_DETACHED -> if (device != null && (device.deviceName == selectedName || device.deviceName == pendingName)) {
                    invalidate()
                    _state.value = UsbProbeState(message = "Scanner unplugged. Reconnect it to resume.")
                }
                permissionAction -> {
                    if (device == null || device.deviceName != pendingName || intent.getStringExtra("token") != permissionToken) return
                    permissionIntent?.cancel(); permissionIntent = null; pendingName = null; permissionToken = null
                    if (manager.hasPermission(device) && isProbe(device)) open(device)
                    else _state.value = UsbProbeState(message = "USB access was denied. Tap Connect USB to try again.")
                }
            }
        }
    }

    override fun start() {
        if (active) return
        active = true
        ContextCompat.registerReceiver(context, receiver, IntentFilter().apply {
            addAction(permissionAction); addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED); addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }, ContextCompat.RECEIVER_NOT_EXPORTED)
        connect()
    }

    override fun stop() {
        if (!active) return
        active = false
        context.unregisterReceiver(receiver)
        invalidate()
        _state.value = UsbProbeState()
    }

    private fun invalidate() {
        generation++
        reader?.cancel(); reader = null
        selectedName = null; pendingName = null; permissionToken = null
        permissionIntent?.cancel(); permissionIntent = null
    }

    private fun isProbe(device: UsbDevice) = isUsbProbeProduct(device.vendorId, device.productId,
        runCatching { device.productName }.getOrNull())

    override fun connect() {
        if (!active || reader?.isActive == true || pendingName != null) return
        val devices = manager.deviceList.values.filter(::isProbe)
        if (devices.size != 1) {
            _state.value = UsbProbeState(message = when {
                devices.size > 1 -> "More than one probe scanner is attached. Connect one scanner at a time."
                manager.deviceList.values.any { it.vendorId == PROBE_USB_VENDOR } -> "ESP32 detected. Flash Wi-Fi Probe firmware, then use its native USB port."
                else -> UsbProbeState().message
            })
            return
        }
        val device = devices.single()
        if (manager.hasPermission(device)) open(device)
        else {
            pendingName = device.deviceName
            permissionToken = UUID.randomUUID().toString()
            permissionIntent = PendingIntent.getBroadcast(context, generation, Intent(permissionAction).setPackage(context.packageName)
                .putExtra("token", permissionToken), PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_CANCEL_CURRENT)
            _state.value = UsbProbeState(connecting = true, message = "Allow USB access to connect the scanner.")
            manager.requestPermission(device, permissionIntent)
        }
    }

    private fun open(device: UsbDevice) {
        invalidate()
        selectedName = device.deviceName
        val ticket = generation
        _state.value = UsbProbeState(connecting = true, message = "Waiting for probe scanner…")
        reader = scope.launch {
            // Serialize reconnects until the cancelled reader releases its interfaces.
            try {
                withContext(Dispatchers.IO) { connectionMutex.withLock { read(device, ticket) } }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) {
                if (active && generation == ticket) {
                    selectedName = null
                    _state.value = UsbProbeState(message = "USB connection stopped. Check the cable and tap Connect USB.")
                }
            }
        }
    }

    private suspend fun publish(ticket: Int, value: UsbProbeState) = withContext(Dispatchers.Main.immediate) {
        if (active && generation == ticket) _state.value = value
    }

    private suspend fun read(device: UsbDevice, ticket: Int) {
        val control = (0 until device.interfaceCount).map(device::getInterface)
            .first { it.interfaceClass == UsbConstants.USB_CLASS_COMM }
        val data = (0 until device.interfaceCount).map(device::getInterface)
            .first { it.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA }
        val input = (0 until data.endpointCount).map(data::getEndpoint)
            .first { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK && it.direction == UsbConstants.USB_DIR_IN }
        val connection = checkNotNull(manager.openDevice(device))
        try {
            check(connection.claimInterface(control, true) && connection.claimInterface(data, true))
            // CDC 115200/8N1, DTR + RTS. The native USB transport itself is not baud limited.
            check(connection.controlTransfer(0x21, 0x20, 0, control.id, byteArrayOf(0, 0xc2.toByte(), 1, 0, 0, 0, 8), 7, 1000) == 7)
            check(connection.controlTransfer(0x21, 0x22, 3, control.id, null, 0, 1000) >= 0)
            val framing = UsbProbeLineFramer()
            val session = UsbProbeSession()
            val bytes = ByteArray(2048)
            val started = clock.nowElapsedMs()
            var published = started - 500
            while (currentCoroutineContext().isActive) {
                val count = connection.bulkTransfer(input, bytes, bytes.size, 200)
                val now = clock.nowElapsedMs()
                if (count > 0) framing.accept(bytes, count).forEach { session.accept(it, now, clock.nowWallClock().toEpochMilli() / 1000.0) }
                if (now - (session.lastHeartbeatMs ?: started) > 8000) error("Probe heartbeat missing")
                if (now - published >= 500 && session.sensorId != null) {
                    publish(ticket, UsbProbeState(connected = true,
                        message = "USB live · 2.4 GHz · channels 1–13 · firmware ${session.version}",
                        snapshot = session.snapshot(now), receivedMs = now, dropped = session.dropped))
                    published = now
                }
            }
        } finally {
            connection.controlTransfer(0x21, 0x22, 0, control.id, null, 0, 200)
            connection.releaseInterface(data); connection.releaseInterface(control); connection.close()
        }
    }
}
