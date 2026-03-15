package com.friendorfoe.detection

import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.aware.AttachCallback
import android.net.wifi.aware.DiscoverySessionCallback
import android.net.wifi.aware.PeerHandle
import android.net.wifi.aware.SubscribeConfig
import android.net.wifi.aware.SubscribeDiscoverySession
import android.net.wifi.aware.WifiAwareManager
import android.net.wifi.aware.WifiAwareSession
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.friendorfoe.domain.model.Drone
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * WiFi Aware (NaN) Remote ID scanner for detecting drones broadcasting
 * OpenDroneID over WiFi Neighbor Awareness Networking.
 *
 * ASTM F3411-22a defines three transports for Remote ID: BLE, WiFi NaN,
 * and WiFi Beacon. DJI drones in particular often use WiFi NaN.
 *
 * The scanner subscribes to the "_odid._udp" service type and parses
 * the service-specific info bytes using the same [OpenDroneIdParser]
 * shared with the BLE [RemoteIdScanner].
 *
 * Gracefully degrades on devices that lack WiFi Aware hardware.
 */
@Singleton
class WifiNanRemoteIdScanner @Inject constructor(
    @ApplicationContext private val context: Context
) {

    companion object {
        private const val TAG = "WifiNanRemoteIdScanner"

        /** ASTM F3411 NaN service name for OpenDroneID */
        private const val ODID_SERVICE_NAME = "_odid._udp"
    }

    // Track parsed data per NaN peer. Like BLE, messages may arrive in
    // separate service discovery events.
    private val droneStates = mutableMapOf<String, OpenDroneIdParser.DronePartialState>()

    /**
     * Start scanning for OpenDroneID broadcasts over WiFi NaN.
     *
     * Returns a Flow that emits Drone objects. If WiFi Aware is not supported
     * or not available, the flow completes immediately without emitting.
     */
    fun startScanning(): Flow<Drone> = callbackFlow {
        // Check hardware support
        if (!context.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_AWARE)) {
            Log.w(TAG, "WiFi Aware not supported on this device")
            close()
            return@callbackFlow
        }

        val awareManager = context.getSystemService(Context.WIFI_AWARE_SERVICE)
            as? WifiAwareManager
        if (awareManager == null || !awareManager.isAvailable) {
            Log.w(TAG, "WiFi Aware not available")
            close()
            return@callbackFlow
        }

        val handler = Handler(Looper.getMainLooper())
        var awareSession: WifiAwareSession? = null
        var discoverySession: SubscribeDiscoverySession? = null

        val discoveryCallback = object : DiscoverySessionCallback() {
            override fun onServiceDiscovered(
                peerHandle: PeerHandle,
                serviceSpecificInfo: ByteArray?,
                matchFilter: MutableList<ByteArray>?
            ) {
                try {
                    val drone = processDiscovery(peerHandle, serviceSpecificInfo, matchFilter)
                    if (drone != null) {
                        trySend(drone)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing NaN discovery", e)
                }
            }

            override fun onServiceDiscoveredWithinRange(
                peerHandle: PeerHandle,
                serviceSpecificInfo: ByteArray?,
                matchFilter: MutableList<ByteArray>?,
                distanceMm: Int
            ) {
                try {
                    val rttDistanceM = distanceMm / 1000.0
                    Log.d(TAG, "NaN RTT range: ${rttDistanceM}m for peer ${peerHandle.hashCode()}")
                    val drone = processDiscovery(peerHandle, serviceSpecificInfo, matchFilter, rttDistanceM)
                    if (drone != null) {
                        trySend(drone)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing NaN ranged discovery", e)
                }
            }

            override fun onSubscribeStarted(session: SubscribeDiscoverySession) {
                discoverySession = session
                Log.i(TAG, "WiFi NaN OpenDroneID subscription started")
            }

            override fun onSessionTerminated() {
                Log.w(TAG, "WiFi NaN discovery session terminated")
                discoverySession = null
            }
        }

        val attachCallback = object : AttachCallback() {
            override fun onAttached(session: WifiAwareSession) {
                awareSession = session
                Log.i(TAG, "WiFi Aware attached, subscribing to OpenDroneID")

                val subscribeConfig = SubscribeConfig.Builder()
                    .setServiceName(ODID_SERVICE_NAME)
                    .setSubscribeType(SubscribeConfig.SUBSCRIBE_TYPE_PASSIVE)
                    .build()

                session.subscribe(subscribeConfig, discoveryCallback, handler)
            }

            override fun onAttachFailed() {
                Log.e(TAG, "WiFi Aware attach failed")
            }

            override fun onAwareSessionTerminated() {
                Log.w(TAG, "WiFi Aware session terminated")
                awareSession = null
            }
        }

        Log.i(TAG, "Starting WiFi NaN Remote ID scan")
        awareManager.attach(attachCallback, handler)

        awaitClose {
            Log.i(TAG, "Stopping WiFi NaN Remote ID scan")
            try {
                discoverySession?.close()
            } catch (e: Exception) {
                Log.w(TAG, "Error closing discovery session", e)
            }
            try {
                awareSession?.close()
            } catch (e: Exception) {
                Log.w(TAG, "Error closing WiFi Aware session", e)
            }
            droneStates.clear()
        }
    }

    /** Stop scanning (for imperative callers). */
    fun stopScanning() {
        droneStates.clear()
    }

    /**
     * Process a WiFi NaN service discovery event.
     *
     * The serviceSpecificInfo contains the raw OpenDroneID message bytes.
     * Some implementations also place message fragments in matchFilter entries.
     */
    private fun processDiscovery(
        peerHandle: PeerHandle,
        serviceSpecificInfo: ByteArray?,
        matchFilter: MutableList<ByteArray>?,
        rttDistanceMeters: Double? = null
    ): Drone? {
        // Try serviceSpecificInfo first (primary location for OpenDroneID payload)
        val messageData = extractMessageData(serviceSpecificInfo)
            ?: extractMessageFromMatchFilter(matchFilter)
            ?: return null

        val now = Instant.now()
        val peerId = "nan_peer_${peerHandle.hashCode()}"

        val state = droneStates.getOrPut(peerId) {
            OpenDroneIdParser.DronePartialState(deviceAddress = peerId, firstSeen = now)
        }
        state.lastUpdated = now

        // Use RTT distance when available (much more accurate than RSSI estimation)
        if (rttDistanceMeters != null) {
            state.rttDistanceMeters = rttDistanceMeters
        }

        OpenDroneIdParser.parseMessage(messageData, state)

        return state.toDroneOrNull(idPrefix = "nan_")
    }

    /**
     * Extract a 25-byte OpenDroneID message from serviceSpecificInfo.
     *
     * The payload may be the raw 25 bytes, or prefixed with an app code byte.
     */
    private fun extractMessageData(serviceSpecificInfo: ByteArray?): ByteArray? {
        if (serviceSpecificInfo == null || serviceSpecificInfo.isEmpty()) return null

        return if (serviceSpecificInfo.size >= 26) {
            // Skip leading app-code byte
            serviceSpecificInfo.copyOfRange(1, serviceSpecificInfo.size)
        } else if (serviceSpecificInfo.size >= 25) {
            serviceSpecificInfo
        } else {
            Log.d(TAG, "serviceSpecificInfo too short: ${serviceSpecificInfo.size} bytes")
            null
        }
    }

    /**
     * Some implementations place OpenDroneID messages in matchFilter entries.
     * Look for a byte array that's at least 25 bytes.
     */
    private fun extractMessageFromMatchFilter(matchFilter: MutableList<ByteArray>?): ByteArray? {
        if (matchFilter == null) return null
        for (entry in matchFilter) {
            if (entry.size >= 25) {
                return entry
            }
        }
        return null
    }
}
