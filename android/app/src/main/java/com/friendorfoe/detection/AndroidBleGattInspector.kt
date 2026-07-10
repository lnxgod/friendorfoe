package com.friendorfoe.detection

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred

@Singleton
class AndroidBleGattInspector @Inject constructor(
    @ApplicationContext private val context: Context,
    private val bluetoothManager: BluetoothManager,
) : BleInvestigator {
    private val activeLock = Any()
    private var activeSession: GattSession? = null

    @SuppressLint("MissingPermission")
    override suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult {
        validateRequest(request)?.let { return it }

        val adapter = bluetoothManager.adapter
            ?: return failedResult(request, "Bluetooth is unavailable", "bluetooth_unavailable")
        if (!adapter.isEnabled) {
            return failedResult(request, "Bluetooth is disabled", "bluetooth_disabled")
        }

        val device = try {
            adapter.getRemoteDevice(request.target.mac)
        } catch (_: IllegalArgumentException) {
            return failedResult(request, "Target address is invalid", "invalid_target")
        } catch (_: SecurityException) {
            return failedResult(
                request,
                "Bluetooth connect permission is required",
                "bluetooth_connect_permission_required",
            )
        }
        val initialBondState = try {
            device.bondState
        } catch (_: SecurityException) {
            return failedResult(
                request,
                "Bluetooth connect permission is required",
                "bluetooth_connect_permission_required",
            )
        }
        if (initialBondState == BluetoothDevice.BOND_BONDING) {
            return failedResult(request, "Target bond state is changing", "bond_state_unstable")
        }

        val session = GattSession(
            context = context,
            request = request,
            device = device,
            initialBondState = initialBondState,
        )
        val claimed = synchronized(activeLock) {
            if (activeSession == null) {
                activeSession = session
                true
            } else {
                false
            }
        }
        if (!claimed) {
            return failedResult(request, "Another BLE investigation is active", "busy")
        }

        return try {
            session.investigate(progress)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: SecurityException) {
            failedResult(
                request,
                "Bluetooth connect permission was revoked",
                "bluetooth_connect_permission_required",
            )
        } catch (_: Exception) {
            failedResult(request, "GATT inspection failed", "gatt_error")
        } finally {
            session.closeOnce()
            synchronized(activeLock) {
                if (activeSession === session) activeSession = null
            }
        }
    }

    override suspend fun cancel() {
        synchronized(activeLock) { activeSession }?.cancel()
    }

    private fun validateRequest(request: BleInvestigationRequest): BleInvestigationResult? {
        if (request.route != BleInvestigationRoute.PHONE) {
            return failedResult(request, "Phone route required", "invalid_route")
        }
        if (request.target.mode != BleInvestigationMode.GATT) {
            return failedResult(request, "GATT mode required", "invalid_mode")
        }
        val mac = request.target.mac
        if (mac == null || !BluetoothAdapter.checkBluetoothAddress(mac)) {
            return failedResult(request, "Target address is invalid", "invalid_target")
        }
        if (!isBleInvestigationTargetFresh(request.target.observedAtElapsedMs, elapsedRealtimeMs())) {
            return failedResult(request, "Target observation is stale", "stale_target")
        }
        if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return failedResult(
                request,
                "Bluetooth connect permission is required",
                "bluetooth_connect_permission_required",
            )
        }
        return null
    }

    private class GattSession(
        private val context: Context,
        private val request: BleInvestigationRequest,
        private val device: BluetoothDevice,
        private val initialBondState: Int,
    ) {
        private val callbackLock = Any()
        private val lifecycleLock = Any()
        private val cancelled = AtomicBoolean(false)
        private val closed = AtomicBoolean(false)
        private val bondTransitioned = AtomicBoolean(false)
        private val services = mutableListOf<String>()
        private val characteristics = mutableListOf<BleGattCharacteristicInfo>()
        private val reads = linkedMapOf<String, String>()

        private var gatt: BluetoothGatt? = null
        private var pendingCallback: PendingCallback? = null
        private var receiverRegistered = false
        private var connected = false
        private var authenticationRequired = false
        private var truncated = false
        private var terminalized = false

        private val callback = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                deliver(GattEvent.Connection(status, newState))
            }

            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                deliver(GattEvent.ServicesDiscovered(status))
            }

            @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
            override fun onCharacteristicRead(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                status: Int,
            ) {
                val value = characteristic.value?.copyOf() ?: byteArrayOf()
                deliver(GattEvent.CharacteristicRead(characteristic, value, status))
            }

            override fun onCharacteristicRead(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                value: ByteArray,
                status: Int,
            ) {
                deliver(GattEvent.CharacteristicRead(characteristic, value.copyOf(), status))
            }
        }

        private val bondReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                val changedDevice = intent.bluetoothDeviceExtra() ?: return
                if (!changedDevice.address.equals(device.address, ignoreCase = true)) return
                val newState = intent.getIntExtra(
                    BluetoothDevice.EXTRA_BOND_STATE,
                    BluetoothDevice.ERROR,
                )
                val changed = synchronized(lifecycleLock) {
                    if (
                        closed.get() ||
                        terminalized ||
                        newState == BluetoothDevice.ERROR ||
                        newState == initialBondState
                    ) {
                        false
                    } else {
                        bondTransitioned.set(true)
                        true
                    }
                }
                if (changed) {
                    deliver(GattEvent.BondStateChanged)
                }
            }
        }

        @SuppressLint("MissingPermission")
        suspend fun investigate(
            progress: suspend (BleInvestigationResult) -> Unit,
        ): BleInvestigationResult {
            progress(result(BleInvestigationState.CONNECTING, "Connecting to BLE target"))
            if (!registerBondReceiver()) return cancelledResult()

            val connection = expect(CallbackKind.CONNECTION)
            val connectedGatt = synchronized(lifecycleLock) {
                if (cancelled.get() || closed.get() || bondTransitioned.get()) {
                    null
                } else {
                    device.connectGatt(
                        context,
                        false,
                        callback,
                        BluetoothDevice.TRANSPORT_LE,
                    )?.also { gatt = it }
                }
            } ?: return when {
                cancelled.get() || closed.get() -> cancelledResult()
                bondTransitioned.get() -> unexpectedBondFailure()
                else -> failed("GATT connection could not start", "connect_failed")
            }

            when (val event = connection.await()) {
                is GattEvent.Connection -> {
                    event.authenticationResult()?.let { return it }
                    if (
                        event.status != BluetoothGatt.GATT_SUCCESS ||
                        event.newState != BluetoothProfile.STATE_CONNECTED
                    ) {
                        return failed("GATT connection failed", "connect_failed")
                    }
                }
                GattEvent.BondStateChanged -> return unexpectedBondFailure()
                GattEvent.Cancelled -> return cancelledResult()
                else -> return failed("Unexpected connection callback", "gatt_callback_error")
            }
            if (hasUnexpectedBondTransition()) return unexpectedBondFailure()
            connected = true

            progress(result(BleInvestigationState.DISCOVERING, "Discovering GATT services"))
            val discovery = expect(CallbackKind.SERVICES)
            when (startGattOperation { it.discoverServices() }) {
                OperationStart.CANCELLED -> return cancelledResult()
                OperationStart.BOND_CHANGED -> return unexpectedBondFailure()
                OperationStart.FAILED ->
                    return failed("Service discovery could not start", "discovery_start_failed")
                OperationStart.STARTED -> Unit
            }
            when (val event = discovery.await()) {
                is GattEvent.ServicesDiscovered -> {
                    event.authenticationResult()?.let { return it }
                    if (event.status != BluetoothGatt.GATT_SUCCESS) {
                        return failed("Service discovery failed", "discovery_failed")
                    }
                }
                is GattEvent.Connection -> {
                    event.authenticationResult()?.let { return it }
                    return failed("Target disconnected during discovery", "disconnected")
                }
                GattEvent.BondStateChanged -> return unexpectedBondFailure()
                GattEvent.Cancelled -> return cancelledResult()
                else -> return failed("Unexpected discovery callback", "gatt_callback_error")
            }
            if (hasUnexpectedBondTransition()) return unexpectedBondFailure()

            val records = snapshotGatt(connectedGatt)
            val candidates = mutableListOf<CharacteristicRecord>()
            for (record in records) {
                val readable = record.characteristic.properties and
                    BluetoothGattCharacteristic.PROPERTY_READ != 0
                val requiresEncryption = record.characteristic.permissions and READ_ENCRYPTION_PERMISSIONS != 0
                when (
                    bleReadDecision(
                        serviceUuid = record.serviceUuid,
                        characteristicUuid = normalizedUuid(record.characteristic.uuid.toString()),
                        readable = readable,
                        requiresEncryption = requiresEncryption,
                    )
                ) {
                    BleReadDecision.READ -> candidates += record
                    BleReadDecision.AUTHENTICATION_REQUIRED -> authenticationRequired = true
                    BleReadDecision.SKIP -> Unit
                }
            }
            val prioritizedCandidates = candidates.sortedBy { it.readPriority() }
            if (prioritizedCandidates.size > MAX_READS) truncated = true

            progress(result(BleInvestigationState.READING, "Reading allowlisted GATT characteristics"))
            for (record in prioritizedCandidates.take(MAX_READS)) {
                if (hasUnexpectedBondTransition()) return unexpectedBondFailure()
                val read = expect(CallbackKind.READ, record.characteristic)
                when (startGattOperation { it.readCharacteristic(record.characteristic) }) {
                    OperationStart.CANCELLED -> return cancelledResult()
                    OperationStart.BOND_CHANGED -> return unexpectedBondFailure()
                    OperationStart.FAILED ->
                        return failed("Characteristic read could not start", "read_start_failed")
                    OperationStart.STARTED -> Unit
                }
                when (val event = read.await()) {
                    is GattEvent.CharacteristicRead -> {
                        if (isAuthenticationStatus(event.status)) {
                            authenticationRequired = true
                            return completed("Authentication required; read-only inspection stopped")
                        }
                        if (event.status != BluetoothGatt.GATT_SUCCESS) {
                            return failed("Characteristic read failed", "read_failed")
                        }
                        recordRead(record, event.value)
                    }
                    is GattEvent.Connection -> {
                        event.authenticationResult()?.let { return it }
                        return failed("Target disconnected during read", "disconnected")
                    }
                    GattEvent.BondStateChanged -> return unexpectedBondFailure()
                    GattEvent.Cancelled -> return cancelledResult()
                    else -> return failed("Unexpected read callback", "gatt_callback_error")
                }
            }

            return completed(
                if (authenticationRequired) {
                    "Read-only GATT inspection complete; protected characteristics skipped"
                } else {
                    "Read-only GATT inspection complete"
                },
            )
        }

        fun cancel() {
            val shouldCancel = synchronized(lifecycleLock) {
                if (cancelled.get() || terminalized) {
                    false
                } else {
                    cancelled.set(true)
                    true
                }
            }
            if (!shouldCancel) return
            deliver(GattEvent.Cancelled)
            closeOnce()
        }

        @SuppressLint("MissingPermission")
        fun closeOnce() {
            var unregisterReceiver = false
            var currentGatt: BluetoothGatt? = null
            val shouldClose = synchronized(lifecycleLock) {
                if (!closed.compareAndSet(false, true)) {
                    false
                } else {
                    unregisterReceiver = receiverRegistered
                    receiverRegistered = false
                    currentGatt = gatt
                    gatt = null
                    true
                }
            }
            if (!shouldClose) return
            deliver(GattEvent.Cancelled)
            if (unregisterReceiver) {
                try {
                    context.unregisterReceiver(bondReceiver)
                } catch (_: IllegalArgumentException) {
                    // Receiver was already removed by the platform.
                }
            }
            if (currentGatt != null) {
                try {
                    currentGatt?.disconnect()
                } catch (_: Exception) {
                    // Closing still must happen when disconnect reports a platform error.
                } finally {
                    try {
                        currentGatt?.close()
                    } catch (_: Exception) {
                        // The atomic guard prevents a second close attempt.
                    }
                }
            }
        }

        private fun registerBondReceiver(): Boolean = synchronized(lifecycleLock) {
            if (cancelled.get() || closed.get() || terminalized) return@synchronized false
            val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(bondReceiver, filter, Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                context.registerReceiver(bondReceiver, filter)
            }
            receiverRegistered = true
            true
        }

        @SuppressLint("MissingPermission")
        private fun hasUnexpectedBondTransition(): Boolean {
            if (bondTransitioned.get()) return true
            val changed = try {
                device.bondState != initialBondState
            } catch (_: SecurityException) {
                true
            }
            if (changed) {
                synchronized(lifecycleLock) { bondTransitioned.set(true) }
            }
            return changed
        }

        @SuppressLint("MissingPermission")
        private fun startGattOperation(operation: (BluetoothGatt) -> Boolean): OperationStart =
            synchronized(lifecycleLock) {
                when {
                    cancelled.get() || closed.get() -> OperationStart.CANCELLED
                    bondTransitioned.get() -> OperationStart.BOND_CHANGED
                    terminalized -> OperationStart.CANCELLED
                    gatt == null -> OperationStart.FAILED
                    operation(gatt!!) -> OperationStart.STARTED
                    else -> OperationStart.FAILED
                }
            }

        private fun snapshotGatt(gatt: BluetoothGatt): List<CharacteristicRecord> {
            val discoveredServices = gatt.services.toList()
            if (discoveredServices.size > MAX_SERVICES) truncated = true
            val totalCharacteristics = discoveredServices.sumOf { it.characteristics.size }
            if (totalCharacteristics > MAX_CHARACTERISTICS) truncated = true

            val visibleServices = discoveredServices.take(MAX_SERVICES)
            services += visibleServices.map { normalizedUuid(it.uuid.toString()) }
            val records = visibleServices
                .flatMap { service ->
                    val serviceUuid = normalizedUuid(service.uuid.toString())
                    service.characteristics.map { CharacteristicRecord(serviceUuid, it) }
                }
                .take(MAX_CHARACTERISTICS)
            characteristics += records.map { record ->
                BleGattCharacteristicInfo(
                    serviceUuid = record.serviceUuid,
                    uuid = normalizedUuid(record.characteristic.uuid.toString()),
                    properties = characteristicProperties(record.characteristic.properties),
                )
            }
            return records
        }

        private fun recordRead(record: CharacteristicRecord, value: ByteArray) {
            if (value.size > MAX_READ_BYTES) truncated = true
            val uuid = normalizedUuid(record.characteristic.uuid.toString())
            if (reads.containsKey(uuid)) {
                truncated = true
                return
            }
            reads[uuid] = value.take(MAX_READ_BYTES).joinToString(separator = "") { byte ->
                String.format(Locale.US, "%02X", byte.toInt() and 0xFF)
            }
        }

        private fun expect(
            kind: CallbackKind,
            characteristic: BluetoothGattCharacteristic? = null,
        ): CompletableDeferred<GattEvent> {
            val deferred = CompletableDeferred<GattEvent>()
            synchronized(callbackLock) {
                when {
                    cancelled.get() || closed.get() -> deferred.complete(GattEvent.Cancelled)
                    bondTransitioned.get() -> deferred.complete(GattEvent.BondStateChanged)
                    else -> {
                        check(pendingCallback == null) { "A GATT callback is already pending" }
                        pendingCallback = PendingCallback(kind, characteristic, deferred)
                    }
                }
            }
            return deferred
        }

        private fun deliver(event: GattEvent) {
            val deferred = synchronized(callbackLock) {
                val pending = pendingCallback ?: return@synchronized null
                if (!pending.matches(event)) return@synchronized null
                pendingCallback = null
                pending.deferred
            }
            deferred?.complete(event)
        }

        private fun GattEvent.Connection.authenticationResult(): BleInvestigationResult? {
            if (!isAuthenticationStatus(status)) return null
            authenticationRequired = true
            return completed("Authentication required; pairing was not requested")
        }

        private fun GattEvent.ServicesDiscovered.authenticationResult(): BleInvestigationResult? {
            if (!isAuthenticationStatus(status)) return null
            authenticationRequired = true
            return completed("Authentication required; pairing was not requested")
        }

        private fun CharacteristicRecord.readPriority(): Int {
            val characteristicUuid = normalizedUuid(characteristic.uuid.toString())
            return when {
                isGapDeviceName(serviceUuid, characteristicUuid) -> 0
                isSerialCharacteristic(characteristicUuid) -> 1
                else -> 2
            }
        }

        @SuppressLint("MissingPermission")
        private fun completed(summary: String): BleInvestigationResult {
            val decision = synchronized(lifecycleLock) {
                val currentBondChanged = try {
                    device.bondState != initialBondState
                } catch (_: SecurityException) {
                    true
                }
                bleTerminalDecision(
                    cancelled = cancelled.get(),
                    closed = closed.get(),
                    bondTransitioned = bondTransitioned.get(),
                    currentBondChanged = currentBondChanged,
                ).also {
                    when (it) {
                        BleTerminalDecision.COMPLETE -> terminalized = true
                        BleTerminalDecision.BOND_CHANGED -> bondTransitioned.set(true)
                        BleTerminalDecision.CANCELLED -> Unit
                    }
                }
            }
            return when (decision) {
                BleTerminalDecision.COMPLETE -> result(BleInvestigationState.COMPLETE, summary)
                BleTerminalDecision.BOND_CHANGED -> unexpectedBondFailure()
                BleTerminalDecision.CANCELLED -> cancelledResult()
            }
        }

        private fun failed(summary: String, error: String): BleInvestigationResult =
            result(BleInvestigationState.FAILED, summary, error)

        private fun unexpectedBondFailure(): BleInvestigationResult =
            failed("Unexpected bond-state transition; inspection stopped", "bond_state_changed")

        private fun cancelledResult(): BleInvestigationResult =
            result(BleInvestigationState.CANCELLED, "BLE investigation cancelled")

        private fun result(
            state: BleInvestigationState,
            summary: String,
            error: String? = null,
        ): BleInvestigationResult = BleInvestigationResult(
            requestId = request.requestId,
            transport = "phone",
            mode = request.target.mode,
            targetMac = request.target.mac,
            state = state,
            connectable = connected,
            services = services.toList(),
            characteristics = characteristics.toList(),
            reads = reads.toMap(),
            bonded = initialBondState == BluetoothDevice.BOND_BONDED,
            encrypted = false,
            authenticationRequired = authenticationRequired,
            summary = summary,
            error = error,
            truncated = truncated,
        )

        private data class CharacteristicRecord(
            val serviceUuid: String,
            val characteristic: BluetoothGattCharacteristic,
        )

        private data class PendingCallback(
            val kind: CallbackKind,
            val characteristic: BluetoothGattCharacteristic?,
            val deferred: CompletableDeferred<GattEvent>,
        ) {
            fun matches(event: GattEvent): Boolean = when (event) {
                GattEvent.BondStateChanged,
                GattEvent.Cancelled,
                -> true
                is GattEvent.Connection ->
                    kind == CallbackKind.CONNECTION ||
                        event.newState == BluetoothProfile.STATE_DISCONNECTED ||
                        event.status != BluetoothGatt.GATT_SUCCESS
                is GattEvent.ServicesDiscovered -> kind == CallbackKind.SERVICES
                is GattEvent.CharacteristicRead ->
                    kind == CallbackKind.READ && event.characteristic === characteristic
            }
        }

        private enum class CallbackKind { CONNECTION, SERVICES, READ }
        private enum class OperationStart { STARTED, CANCELLED, BOND_CHANGED, FAILED }

        private sealed interface GattEvent {
            data class Connection(val status: Int, val newState: Int) : GattEvent
            data class ServicesDiscovered(val status: Int) : GattEvent
            data class CharacteristicRead(
                val characteristic: BluetoothGattCharacteristic,
                val value: ByteArray,
                val status: Int,
            ) : GattEvent

            data object BondStateChanged : GattEvent
            data object Cancelled : GattEvent
        }
    }

    private companion object {
        const val MAX_SERVICES = 16
        const val MAX_CHARACTERISTICS = 32
        const val MAX_READS = 8
        const val MAX_READ_BYTES = 64
        const val READ_ENCRYPTION_PERMISSIONS =
            BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED or
                BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED_MITM
    }
}

internal const val BLE_INVESTIGATION_TARGET_MAX_AGE_MS = 30_000L

internal enum class BleReadDecision { READ, AUTHENTICATION_REQUIRED, SKIP }
internal enum class BleTerminalDecision { COMPLETE, CANCELLED, BOND_CHANGED }

internal fun isBleInvestigationTargetFresh(
    observedAtElapsedMs: Long,
    nowElapsedMs: Long,
): Boolean =
    observedAtElapsedMs >= 0 &&
        observedAtElapsedMs <= nowElapsedMs &&
        nowElapsedMs - observedAtElapsedMs <= BLE_INVESTIGATION_TARGET_MAX_AGE_MS

internal fun bleReadDecision(
    serviceUuid: String,
    characteristicUuid: String,
    readable: Boolean,
    requiresEncryption: Boolean,
): BleReadDecision {
    val allowlisted =
        isGapDeviceName(serviceUuid, characteristicUuid) ||
            isBluetoothUuid(serviceUuid, "180A") ||
            isSerialCharacteristic(characteristicUuid)
    if (!allowlisted) return BleReadDecision.SKIP
    if (requiresEncryption) return BleReadDecision.AUTHENTICATION_REQUIRED
    return if (readable) BleReadDecision.READ else BleReadDecision.SKIP
}

internal fun bleTerminalDecision(
    cancelled: Boolean,
    closed: Boolean,
    bondTransitioned: Boolean,
    currentBondChanged: Boolean,
): BleTerminalDecision = when {
    bondTransitioned || currentBondChanged -> BleTerminalDecision.BOND_CHANGED
    cancelled || closed -> BleTerminalDecision.CANCELLED
    else -> BleTerminalDecision.COMPLETE
}

private fun isGapDeviceName(serviceUuid: String, characteristicUuid: String): Boolean =
    isBluetoothUuid(serviceUuid, "1800") && isBluetoothUuid(characteristicUuid, "2A00")

private fun isSerialCharacteristic(characteristicUuid: String): Boolean =
    isBluetoothUuid(characteristicUuid, "FFE1") || isBluetoothUuid(characteristicUuid, "FFF1")

private fun isBluetoothUuid(uuid: String, shortUuid: String): Boolean {
    val normalized = uuid.lowercase(Locale.US)
    val short = shortUuid.lowercase(Locale.US)
    return normalized == short || normalized == "0000$short-0000-1000-8000-00805f9b34fb"
}

private fun normalizedUuid(uuid: String): String = uuid.uppercase(Locale.US)

private fun characteristicProperties(properties: Int): Set<String> = buildSet {
    if (properties and BluetoothGattCharacteristic.PROPERTY_BROADCAST != 0) add("broadcast")
    if (properties and BluetoothGattCharacteristic.PROPERTY_READ != 0) add("read")
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) add("write_no_response")
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) add("write")
    if (properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) add("notify")
    if (properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) add("indicate")
    if (properties and BluetoothGattCharacteristic.PROPERTY_SIGNED_WRITE != 0) add("signed_write")
    if (properties and BluetoothGattCharacteristic.PROPERTY_EXTENDED_PROPS != 0) add("extended_properties")
}

private fun failedResult(
    request: BleInvestigationRequest,
    summary: String,
    error: String,
): BleInvestigationResult = BleInvestigationResult(
    requestId = request.requestId,
    transport = "phone",
    mode = request.target.mode,
    targetMac = request.target.mac,
    state = BleInvestigationState.FAILED,
    connectable = null,
    services = emptyList(),
    characteristics = emptyList(),
    reads = emptyMap(),
    bonded = false,
    encrypted = false,
    authenticationRequired = false,
    summary = summary,
    error = error,
    truncated = false,
)

@Suppress("DEPRECATION")
private fun Intent.bluetoothDeviceExtra(): BluetoothDevice? =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
    } else {
        getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
    }

private fun isAuthenticationStatus(status: Int): Boolean =
    status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION ||
        status == BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION ||
        status == GATT_INSUFFICIENT_AUTHORIZATION_STATUS

private const val GATT_INSUFFICIENT_AUTHORIZATION_STATUS = 8
