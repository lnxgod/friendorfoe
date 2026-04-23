package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/**
 * BLE peripheral that advertises a known service UUID + name during a
 * phone-driven calibration walk. Sensors hear the advertisement, report
 * RSSI to the backend, the backend joins to the phone's GPS trace and
 * fits a per-listener path-loss model.
 *
 * Why not include manufacturer data with the operator label? On Android
 * the advertisement payload is hard-capped at 31 bytes; reserving most
 * of that for a name/UUID gives the scanner a stable identity. The
 * backend ties the session to a phone via the UUID alone (assigned at
 * /walk/start), so no extra bytes are needed in the payload.
 */
@Singleton
class BleCalibrationAdvertiser @Inject constructor(
    private val bluetoothManager: BluetoothManager
) : CalibrationAdvertiser {
    companion object {
        private const val TAG = "BleCalAdvertiser"
    }

    private var advertiser: BluetoothLeAdvertiser? = null
    private var activeCallback: AdvertiseCallback? = null

    @get:Synchronized
    var isAdvertising: Boolean = false
        private set

    @SuppressLint("MissingPermission")
    override fun start(serviceUuid: String, onError: (String) -> Unit): Boolean {
        stop()
        val adapter = bluetoothManager.adapter
        if (adapter == null || !adapter.isEnabled) {
            onError("Bluetooth is off — enable it and try again.")
            return false
        }
        if (!adapter.isMultipleAdvertisementSupported) {
            onError("This device's Bluetooth chip does not support BLE advertising.")
            return false
        }
        val adv = adapter.bluetoothLeAdvertiser
        if (adv == null) {
            onError("BluetoothLeAdvertiser unavailable.")
            return false
        }
        advertiser = adv

        val uuid = try { UUID.fromString(serviceUuid) } catch (e: Exception) {
            onError("Bad service UUID: ${e.message}")
            return false
        }

        // ADVERTISE_TX_POWER_HIGH gives the most consistent RSSI floor
        // across vendors — Gemini's recommendation, also matches what
        // the backend's DEFAULT_PHONE_TX_DBM (-59 @ 1m) was calibrated for.
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(false)  // pure beacon — no GATT server needed
            .setTimeout(0)          // run until stop() is called
            .build()

        // Service UUID + included TX power. Skip the device name so the
        // 31-byte advertisement budget has room for the full UUID.
        val data = AdvertiseData.Builder()
            .addServiceUuid(ParcelUuid(uuid))
            .setIncludeTxPowerLevel(true)
            .setIncludeDeviceName(false)
            .build()

        val callback = object : AdvertiseCallback() {
            override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
                Log.i(TAG, "BLE advertise started: uuid=$uuid")
                synchronized(this@BleCalibrationAdvertiser) { isAdvertising = true }
            }
            override fun onStartFailure(errorCode: Int) {
                val msg = when (errorCode) {
                    ADVERTISE_FAILED_DATA_TOO_LARGE -> "Advertisement payload too large."
                    ADVERTISE_FAILED_TOO_MANY_ADVERTISERS -> "Too many active advertisers — restart the app."
                    ADVERTISE_FAILED_ALREADY_STARTED -> "Already advertising."
                    ADVERTISE_FAILED_INTERNAL_ERROR -> "BLE internal error."
                    ADVERTISE_FAILED_FEATURE_UNSUPPORTED -> "Device does not support BLE advertising."
                    else -> "BLE advertise failure $errorCode"
                }
                Log.e(TAG, "BLE advertise failed: $msg")
                synchronized(this@BleCalibrationAdvertiser) { isAdvertising = false }
                onError(msg)
            }
        }
        return try {
            adv.startAdvertising(settings, data, callback)
            activeCallback = callback
            true
        } catch (e: SecurityException) {
            onError("Missing BLUETOOTH_ADVERTISE permission.")
            false
        } catch (e: Exception) {
            onError("Could not start advertise: ${e.message}")
            false
        }
    }

    @SuppressLint("MissingPermission")
    @Synchronized
    override fun stop() {
        val cb = activeCallback ?: return
        try {
            advertiser?.stopAdvertising(cb)
        } catch (e: Exception) {
            Log.w(TAG, "stopAdvertising threw: ${e.message}")
        }
        activeCallback = null
        isAdvertising = false
    }
}
