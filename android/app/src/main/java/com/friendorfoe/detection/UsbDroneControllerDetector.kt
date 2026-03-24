package com.friendorfoe.detection

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Detects drone controllers connected via USB OTG.
 *
 * When a DJI, Autel, or other drone controller is plugged into the phone,
 * this detector identifies it by USB Vendor ID (VID) and Product ID (PID).
 *
 * This tells the app "the user is operating a drone" — useful for:
 * - Compliance logging (user is a drone operator, not just an observer)
 * - Adjusting detection behavior (suppress self-detection)
 * - Showing operator-specific UI elements
 *
 * Known USB Vendor IDs:
 * - DJI: 0x2CA3 (all RC-N1, RC Pro, Smart Controller)
 * - Autel: 0x2D99 (EVO Smart Controller)
 * - Parrot: 0x19CF (Skycontroller)
 */
class UsbDroneControllerDetector(private val context: Context) {

    companion object {
        private const val TAG = "UsbDroneCtrl"

        /** Known drone controller USB Vendor IDs */
        private val DRONE_CONTROLLER_VIDS = mapOf(
            0x2CA3 to "DJI",
            0x2D99 to "Autel",
            0x19CF to "Parrot",
            0x0483 to "STMicroelectronics",  // Used by some Herelink/CubePilot controllers
        )
    }

    data class ConnectedController(
        val manufacturer: String,
        val productName: String?,
        val vendorId: Int,
        val productId: Int
    )

    private val _connectedController = MutableStateFlow<ConnectedController?>(null)
    /** Currently connected drone controller, or null if none detected */
    val connectedController: StateFlow<ConnectedController?> = _connectedController.asStateFlow()

    private var receiver: BroadcastReceiver? = null

    /**
     * Start monitoring for USB drone controller connections.
     * Call from Activity/ViewModel lifecycle.
     */
    fun startMonitoring() {
        // Check already-connected devices
        checkConnectedDevices()

        // Register for future attach/detach events
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }

        receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                when (intent.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                        val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                        if (device != null) checkDevice(device)
                    }
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                        val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                        if (device != null && isDroneController(device)) {
                            Log.i(TAG, "Drone controller disconnected: ${device.productName}")
                            _connectedController.value = null
                        }
                    }
                }
            }
        }

        context.registerReceiver(receiver, filter)
        Log.d(TAG, "USB drone controller monitoring started")
    }

    fun stopMonitoring() {
        receiver?.let {
            try {
                context.unregisterReceiver(it)
            } catch (e: Exception) {
                Log.w(TAG, "Error unregistering USB receiver", e)
            }
        }
        receiver = null
    }

    private fun checkConnectedDevices() {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return
        for ((_, device) in usbManager.deviceList) {
            checkDevice(device)
        }
    }

    private fun checkDevice(device: UsbDevice) {
        val manufacturer = DRONE_CONTROLLER_VIDS[device.vendorId]
        if (manufacturer != null) {
            val controller = ConnectedController(
                manufacturer = manufacturer,
                productName = device.productName,
                vendorId = device.vendorId,
                productId = device.productId
            )
            _connectedController.value = controller
            Log.i(TAG, "Drone controller detected: $manufacturer ${device.productName} " +
                    "(VID=0x${device.vendorId.toString(16)}, PID=0x${device.productId.toString(16)})")
        }
    }

    private fun isDroneController(device: UsbDevice): Boolean =
        device.vendorId in DRONE_CONTROLLER_VIDS
}
