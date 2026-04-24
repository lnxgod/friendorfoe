package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.os.ParcelUuid
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

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
    override suspend fun start(serviceUuid: String): Result<Unit> = withContext(Dispatchers.Main) {
        stop()
        val adapter = bluetoothManager.adapter
        if (adapter == null || !adapter.isEnabled) {
            return@withContext Result.failure(
                IllegalStateException("Bluetooth is off — enable it and try again.")
            )
        }
        if (!adapter.isMultipleAdvertisementSupported) {
            return@withContext Result.failure(
                IllegalStateException("This device's Bluetooth chip does not support BLE advertising.")
            )
        }
        val adv = adapter.bluetoothLeAdvertiser
        if (adv == null) {
            return@withContext Result.failure(
                IllegalStateException("BluetoothLeAdvertiser unavailable.")
            )
        }
        advertiser = adv

        val uuid = try {
            UUID.fromString(serviceUuid)
        } catch (e: Exception) {
            return@withContext Result.failure(
                IllegalStateException("Bad service UUID: ${e.message}")
            )
        }

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(false)
            .setTimeout(0)
            .build()

        val data = AdvertiseData.Builder()
            .addServiceUuid(ParcelUuid(uuid))
            .setIncludeTxPowerLevel(true)
            .setIncludeDeviceName(false)
            .build()

        try {
            suspendCancellableCoroutine { cont ->
                val callback = object : AdvertiseCallback() {
                    override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
                        Log.i(TAG, "BLE advertise started: uuid=$uuid")
                        synchronized(this@BleCalibrationAdvertiser) { isAdvertising = true }
                        if (cont.isActive) {
                            cont.resume(Result.success(Unit))
                        }
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
                        synchronized(this@BleCalibrationAdvertiser) {
                            isAdvertising = false
                            activeCallback = null
                        }
                        if (cont.isActive) {
                            cont.resume(Result.failure(IllegalStateException(msg)))
                        }
                    }
                }
                activeCallback = callback
                try {
                    adv.startAdvertising(settings, data, callback)
                } catch (e: SecurityException) {
                    activeCallback = null
                    cont.resume(Result.failure(
                        IllegalStateException("Missing BLUETOOTH_ADVERTISE permission.")
                    ))
                } catch (e: Exception) {
                    activeCallback = null
                    cont.resume(Result.failure(
                        IllegalStateException("Could not start advertise: ${e.message}")
                    ))
                }
                cont.invokeOnCancellation {
                    try {
                        adv.stopAdvertising(callback)
                    } catch (_: Exception) {
                    }
                    synchronized(this@BleCalibrationAdvertiser) {
                        if (activeCallback === callback) {
                            activeCallback = null
                        }
                        isAdvertising = false
                    }
                }
            }
        } catch (e: Exception) {
            Result.failure(e)
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
