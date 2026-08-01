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
import android.util.Log
import androidx.core.content.ContextCompat
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.IOException
import java.time.Instant
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import kotlin.coroutines.resume

enum class BadgeUsbStatus {
    DISCONNECTED,
    PERMISSION_NEEDED,
    CONNECTING,
    AP_CONNECTED,
    DEBUG_BRIDGE_CONNECTED,
    BLE_CONNECTED,
    CONNECTED,
    ERROR,
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
    val rssi: Int,
)

private enum class UsbLineWriteResult {
    NOT_ATTEMPTED,
    WRITTEN,
    ATTEMPTED_UNCERTAIN,
}

@Singleton
class BadgeUsbRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val usbManager: UsbManager,
    private val httpClients: BadgeHttpClients,
    private val debugBridgeConfig: BadgeDebugBridgeConfig,
    private val certification: BadgeReleaseCertification,
    private val clock: MonotonicClock,
    @ApplicationScope private val scope: CoroutineScope,
) : BadgeControlPort {

    companion object {
        private const val TAG = "BadgeUsbRepository"
        private const val ACTION_USB_PERMISSION = "com.friendorfoe.action.USB_BADGE_PERMISSION"
        private const val EXTRA_PERMISSION_SESSION = "badge_permission_session"
        private const val EXTRA_PERMISSION_TARGET = "badge_permission_target"
        private const val EXTRA_PERMISSION_NONCE = "badge_permission_nonce"
        private const val ESPRESSIF_VENDOR_ID = 0x303A
        private const val READ_TIMEOUT_MS = 250
        private const val WRITE_TIMEOUT_MS = 250
        private const val AP_POLL_INTERVAL_MS = 2_500L
        private const val DEBUG_BRIDGE_POLL_INTERVAL_MS = 1_500L
        private const val BLE_SCAN_INTERVAL_MS = 6_000L
        private const val BLE_SCAN_WINDOW_MS = 4_500L
        private const val BLE_OPERATION_IDLE_TIMEOUT_MS = 1_000L
        private const val USB_STATUS_POLL_INTERVAL_MS = 2_000L
        private const val MAX_RECENT_DETECTIONS = 20
        private const val MAX_LINE_CHARS = 8_192
        private val BADGE_BLE_SERVICE_UUID: UUID =
            UUID.fromString("0000f0f0-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_STATUS_UUID: UUID =
            UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_CONTROL_UUID: UUID =
            UUID.fromString("0000ff02-0000-1000-8000-00805f9b34fb")
        private val CLIENT_CONFIG_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val commandMutex = Mutex()
    private val connectionMutex = Mutex()
    private val lifecycleLock = Any()
    private val started = AtomicBoolean(false)
    private val transportGate = BadgeTransportGenerationGate()
    private val httpCommandStarts = BadgeHttpCommandStartGate(transportGate)
    private val httpStatusRequests = BadgeHttpStatusRequestGate()
    private val usbPermissionRequests = BadgeUsbPermissionRequestGate()
    private val usbCommands = BadgeUsbCommandCoordinator()
    private val bleCommands = BadgeBleCommandCoordinator()
    private val bleGattOperations = BadgeBleGattOperationCoordinator()
    private val bleScanLeases = BadgeBleScanLeaseCoordinator<ScanCallback>()
    private val jsonMediaType = "application/json".toMediaType()

    private val stateStore = BadgeRepositoryStateStore(
        initialState = BadgeRepositoryState(),
        clock = clock,
        scope = scope,
    )
    override val state: StateFlow<BadgeRepositoryState> = stateStore.state

    private val _legacyState = MutableStateFlow(BadgeUsbState())
    val legacyState: StateFlow<BadgeUsbState> = _legacyState.asStateFlow()

    @Volatile private var sessionGeneration = 0L
    @Volatile private var activeTransportToken: BadgeActiveTransportToken? = null
    @Volatile private var activeUsbToken: BadgeActiveTransportToken? = null
    @Volatile private var activeBleToken: BadgeActiveTransportToken? = null
    @Volatile private var activeHttpToken: BadgeActiveTransportToken? = null
    @Volatile private var activeUsbTargetId: String? = null
    @Volatile private var activeUsbCommandGeneration = usbCommands.currentTransportGeneration()
    @Volatile private var activeBleCommandGeneration = bleCommands.currentTransportGeneration()

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
    @Volatile private var negotiatedBleMtu = 23
    @Volatile private var bleMutationInFlight = false

    init {
        scope.launch {
            state.collect { repositoryState ->
                val httpToken = activeHttpToken
                if (httpToken != null &&
                    shouldReleaseExpiredHttpLease(repositoryState.connection, httpToken)
                ) {
                    transportGate.runIfCurrent(httpToken) {
                        if (state.value.connection.phase == BadgeConnectionPhase.EXPIRED &&
                            state.value.connection.transport == httpToken.transport
                        ) {
                            activeHttpToken = null
                            if (activeTransportToken == httpToken) activeTransportToken = null
                            transportGate.release(httpToken)
                        }
                    }
                }
                _legacyState.update { current ->
                    current.copy(
                        status = repositoryState.connection.toLegacyStatus(),
                        controlStatus = repositoryState.controlStatus,
                        detections = repositoryState.detections,
                    )
                }
            }
        }
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device = intent.usbDeviceExtra() ?: return
                    val request = BadgeUsbPermissionRequest(
                        sessionGeneration = intent.getLongExtra(
                            EXTRA_PERMISSION_SESSION,
                            Long.MIN_VALUE,
                        ),
                        targetId = intent.getStringExtra(EXTRA_PERMISSION_TARGET) ?: return,
                        nonce = intent.getLongExtra(EXTRA_PERMISSION_NONCE, Long.MIN_VALUE),
                    )
                    if (!usbPermissionRequests.consume(
                            request = request,
                            currentSessionGeneration = sessionGeneration,
                            resultTargetId = device.usbTargetId(),
                        )
                    ) {
                        return
                    }
                    val session = transportGate.captureSession(request.sessionGeneration)
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        scope.launch { connectToDevice(device, session) }
                    } else {
                        publishPermissionNeeded(device, session)
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> refreshUsb(
                    transportGate.captureSession(sessionGeneration),
                )
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val detached = intent.usbDeviceExtra()
                    if (detached != null && matchesActiveUsbTarget(
                            detachedTargetId = detached.usbTargetId(),
                            activeTargetId = activeUsbTargetId,
                        )
                    ) {
                        disconnectUsb("Badge disconnected", activeUsbToken)
                    }
                }
            }
        }
    }

    override fun start() {
        synchronized(lifecycleLock) {
            if (!started.compareAndSet(false, true)) return
            val session = transportGate.startSession()
            httpStatusRequests.startSession(session)
            sessionGeneration = session
            registerReceiverIfNeeded()
            val sessionToken = transportGate.captureSession(session)
            refreshUsb(sessionToken)
            startBlePoller(sessionToken)
            startApPoller(sessionToken)
            startDebugBridgePoller(sessionToken)
        }
    }

    override fun stop() {
        synchronized(lifecycleLock) {
            if (!started.getAndSet(false)) return
            httpStatusRequests.stopSession()
            transportGate.stopSession()
            sessionGeneration = 0L
            activeTransportToken = null
            activeUsbToken = null
            activeUsbTargetId = null
            activeBleToken = null
            activeHttpToken = null
            usbPermissionRequests.clear()
            activeUsbCommandGeneration = usbCommands.invalidateTransport("Badge transport stopped")
            activeBleCommandGeneration = bleCommands.invalidateTransport("Badge transport stopped")

            apPollJob?.cancel()
            apPollJob = null
            debugBridgePollJob?.cancel()
            debugBridgePollJob = null
            blePollJob?.cancel()
            blePollJob = null
            usbStatusPollJob?.cancel()
            usbStatusPollJob = null
            readJob?.cancel()
            readJob = null
            stopBleScan()
            closeUsbResources()
            closeBleResources()
            if (receiverRegistered) {
                runCatching { context.unregisterReceiver(usbReceiver) }
                receiverRegistered = false
            }
            publishInactivePhase(BadgeConnectionPhase.DISCONNECTED, "Badge control paused")
        }
    }

    override fun requestConnection() {
        val session = transportGate.captureSession(sessionGeneration)
        if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
        registerReceiverIfNeeded()
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            refreshUsb(session)
            return
        }
        if (candidates.size > 1) {
            reportAmbiguousBadgeDevices(candidates, session)
            return
        }
        val device = candidates.first()
        if (usbManager.hasPermission(device)) {
            scope.launch { connectToDevice(device, session) }
            return
        }
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_IMMUTABLE else 0
        val permissionRequest = usbPermissionRequests.issue(
            sessionGeneration = session.sessionGeneration,
            targetId = device.usbTargetId(),
        )
        val permissionIntent = PendingIntent.getBroadcast(
            context,
            permissionRequest.nonce.toInt(),
            Intent(ACTION_USB_PERMISSION)
                .setPackage(context.packageName)
                .putExtra(EXTRA_PERMISSION_SESSION, permissionRequest.sessionGeneration)
                .putExtra(EXTRA_PERMISSION_TARGET, permissionRequest.targetId)
                .putExtra(EXTRA_PERMISSION_NONCE, permissionRequest.nonce),
            flags,
        )
        usbManager.requestPermission(device, permissionIntent)
        publishPermissionNeeded(device, session)
    }

    override fun refreshStatus() {
        requestStatus()
    }

    override suspend fun execute(command: BadgeCommand): BadgeCommandOutcome =
        commandMutex.withLock {
            val validationFailure = when (command) {
                is BadgeCommand.ApplyTheme -> BadgeTheme.validate(command.theme).exceptionOrNull()
                is BadgeCommand.ApplyPolicy -> BadgeDisplayPolicy.validate(command.policy).exceptionOrNull()
                else -> null
            }
            if (validationFailure != null) {
                return@withLock publishOutcome(
                    BadgeCommandOutcome.Failed(validationFailure.message ?: "Badge draft is invalid"),
                )
            }
            val snapshot = state.value
            val required = command.requiredCapability()
            val support = badgeCapability(
                snapshot.connection,
                required,
                command.payloadSizeOrNull(),
            )
            if (support != BadgeCapabilitySupport.SUPPORTED) {
                return@withLock publishOutcome(
                    BadgeCommandOutcome.Unsupported(
                        "${required.name.lowercase()} is unavailable on this connection",
                    ),
                )
            }
            val executionToken = activeTokenFor(snapshot.connection.transport)
            if (executionToken == null || !transportGate.isCurrent(executionToken) ||
                !snapshot.connection.matchesActiveToken(executionToken)
            ) {
                return@withLock BadgeCommandOutcome.Unsupported(
                    "Badge connection changed before the command started",
                )
            }
            val outcome = when (snapshot.connection.transport) {
                BadgeTransport.USB_SERIAL -> executeUsbOnce(command, executionToken)
                BadgeTransport.LOCAL_AP_HTTP -> executeApHttpOnce(command, executionToken)
                BadgeTransport.BLE_GATT -> executeBleOnce(command, executionToken)
                BadgeTransport.DEBUG_BRIDGE -> executeDebugBridgeOnce(command, executionToken)
                null -> BadgeCommandOutcome.Unsupported("No badge transport is active")
            }
            var published = false
            transportGate.runIfCurrent(executionToken) {
                if (command in setOf(BadgeCommand.Reboot, BadgeCommand.EnterBootloader) &&
                    outcome is BadgeCommandOutcome.Acknowledged
                ) {
                    publishRecoveryAcknowledgement(command, outcome)
                } else {
                    publishOutcome(outcome)
                }
                published = true
            }
            if (published) {
                outcome
            } else {
                var expectedRecoveryDisconnectPublished = false
                transportGate.runIfSessionHasNoActiveTransport(
                    executionToken.sessionGeneration,
                ) {
                    if (canPublishExpectedRecoveryDisconnect(
                            command = command,
                            outcome = outcome,
                            attemptedConnection = snapshot.connection,
                            currentConnection = state.value.connection,
                        )
                    ) {
                        publishRecoveryAcknowledgement(command, outcome)
                        expectedRecoveryDisconnectPublished = true
                    }
                }
                if (expectedRecoveryDisconnectPublished) {
                    outcome
                } else {
                    BadgeCommandOutcome.Unsupported(
                        "Badge connection changed before command completion",
                    )
                }
            }
        }

    fun sendPing() {
        scope.launch {
            val token = activeUsbToken ?: return@launch
            writeLine(token, "FOF_PING")
        }
    }

    fun requestStatus() {
        scope.launch {
            val connection = state.value.connection
            when (connection.transport) {
                BadgeTransport.USB_SERIAL -> activeUsbToken?.let { writeLine(it, "FOF_STATUS") }
                BadgeTransport.BLE_GATT -> activeBleToken?.let { readBleStatus(it) }
                BadgeTransport.LOCAL_AP_HTTP -> fetchApStatus(
                    transportGate.captureSession(sessionGeneration),
                    showErrors = true,
                )
                BadgeTransport.DEBUG_BRIDGE -> fetchDebugBridgeStatus(
                    transportGate.captureSession(sessionGeneration),
                    showErrors = true,
                )
                null -> fetchNetworkStatus(
                    transportGate.captureSession(sessionGeneration),
                    showErrors = true,
                )
            }
        }
    }

    fun setMode(mode: BadgeNetworkMode) = launchCommand(BadgeCommand.SetNetworkMode(mode))
    fun rebootBadge() = launchCommand(BadgeCommand.Reboot)
    fun enterBootloader() = launchCommand(BadgeCommand.EnterBootloader)
    fun applyDisplayPolicy(policy: BadgeDisplayPolicy) = launchCommand(BadgeCommand.ApplyPolicy(policy))
    fun resetDisplayPolicy() = launchCommand(
        BadgeCommand.ApplyPolicy(BadgeDisplayPolicy.firmwareDefaults()),
    )
    fun applyBadgeTheme(theme: BadgeTheme) = launchCommand(BadgeCommand.ApplyTheme(theme))
    fun resetBadgeTheme() = launchCommand(BadgeCommand.ApplyTheme(BadgeTheme.firmwareDefaults()))
    fun displayNav(action: BadgeDisplayAction) = launchCommand(BadgeCommand.NavigateDisplay(action))

    private fun launchCommand(command: BadgeCommand) {
        scope.launch { execute(command) }
    }

    private fun activeTokenFor(transport: BadgeTransport?): BadgeActiveTransportToken? =
        when (transport) {
            BadgeTransport.USB_SERIAL -> activeUsbToken
            BadgeTransport.BLE_GATT -> activeBleToken
            BadgeTransport.LOCAL_AP_HTTP,
            BadgeTransport.DEBUG_BRIDGE,
            -> activeHttpToken
            null -> null
        }

    private fun publishOutcome(outcome: BadgeCommandOutcome): BadgeCommandOutcome {
        stateStore.update { it.copy(lastCommandOutcome = outcome) }
        _legacyState.update { current ->
            current.copy(message = outcome.userMessage(current.transportLabel))
        }
        return outcome
    }

    private fun publishRecoveryAcknowledgement(
        command: BadgeCommand,
        outcome: BadgeCommandOutcome,
    ) {
        stateStore.update { it.copy(lastCommandOutcome = outcome) }
        _legacyState.update { current ->
            current.copy(
                message = when (command) {
                    BadgeCommand.Reboot ->
                        "Badge reboot acknowledged; reconnect after restart"
                    BadgeCommand.EnterBootloader ->
                        "Badge bootloader acknowledged; reconnect after recovery"
                    else -> current.message
                },
            )
        }
    }

    private suspend fun executeUsbOnce(
        command: BadgeCommand,
        token: BadgeActiveTransportToken,
    ): BadgeCommandOutcome {
        if (token != activeUsbToken || !transportGate.isCurrent(token)) {
            return BadgeCommandOutcome.Unsupported("Direct USB-C session changed")
        }
        val deferred = CompletableDeferred<BadgeCommandOutcome>()
        var generation = Long.MIN_VALUE
        var commandBegan = false
        transportGate.runIfCurrent(token) {
            if (token != activeUsbToken) return@runIfCurrent
            generation = activeUsbCommandGeneration
            commandBegan = usbCommands.begin(generation, command, deferred)
        }
        if (!commandBegan) {
            return BadgeCommandOutcome.Unsupported(
                "USB mutation path requires a reconnect after an uncertain command",
            )
        }
        var writeAttempted = false
        try {
            when (writeUsbLine(
                token = token,
                line = "FOF_CTL:${command.toControlJson()}",
                onAttempt = { writeAttempted = true },
            )) {
                UsbLineWriteResult.NOT_ATTEMPTED -> {
                    usbCommands.clearExact(deferred)
                    return BadgeCommandOutcome.Failed("Badge USB write did not start")
                }
                UsbLineWriteResult.ATTEMPTED_UNCERTAIN -> {
                    usbCommands.cancelAfterAttempt(generation, command, deferred)
                    return BadgeCommandOutcome.Failed(
                        "Badge USB write was uncertain; reconnect before retrying",
                    )
                }
                UsbLineWriteResult.WRITTEN -> Unit
            }
            val outcome = withTimeoutOrNull(5_000L) { deferred.await() }
            if (outcome == null) {
                usbCommands.timeout(generation, command, deferred)
                return BadgeCommandOutcome.TimedOut
            }
            return outcome
        } catch (cancelled: CancellationException) {
            if (writeAttempted) {
                usbCommands.cancelAfterAttempt(generation, command, deferred)
            } else {
                usbCommands.clearExact(deferred)
            }
            throw cancelled
        } finally {
            usbCommands.clearExact(deferred)
        }
    }

    private suspend fun executeBleOnce(
        command: BadgeCommand,
        token: BadgeActiveTransportToken,
    ): BadgeCommandOutcome {
        if (token != activeBleToken || !transportGate.isCurrent(token)) {
            return BadgeCommandOutcome.Unsupported("Badge BLE session changed")
        }
        val deferred = CompletableDeferred<BadgeCommandOutcome>()
        var generation = Long.MIN_VALUE
        var operationEpoch = Long.MIN_VALUE
        var commandBegan = false
        transportGate.runIfCurrent(token) {
            if (token != activeBleToken) return@runIfCurrent
            generation = activeBleCommandGeneration
            operationEpoch = bleGattOperations.currentEpoch()
            commandBegan = bleCommands.begin(generation, command, deferred)
        }
        if (!commandBegan) {
            return BadgeCommandOutcome.Unsupported(
                "BLE mutation path requires a reconnect after an uncertain command",
            )
        }
        bleMutationInFlight = true
        var operationClaimed = false
        var writeAttempted = false
        try {
            operationClaimed = bleGattOperations.awaitAndBegin(
                operation = BadgeBleGattOperation.CONTROL_WRITE,
                expectedEpoch = operationEpoch,
                timeoutMs = BLE_OPERATION_IDLE_TIMEOUT_MS,
            )
            if (!operationClaimed) {
                bleCommands.clearExact(deferred)
                return BadgeCommandOutcome.Failed("Badge BLE status read is still in progress")
            }
            if (token != activeBleToken || !transportGate.isCurrent(token)) {
                bleGattOperations.complete(BadgeBleGattOperation.CONTROL_WRITE)
                bleCommands.clearExact(deferred)
                return BadgeCommandOutcome.Unsupported("Badge BLE session changed")
            }
            if (!writeBleControl(token, command.toControlJson())) {
                bleGattOperations.complete(BadgeBleGattOperation.CONTROL_WRITE)
                bleCommands.clearExact(deferred)
                return BadgeCommandOutcome.Failed("Badge BLE write failed")
            }
            writeAttempted = true
            val outcome = withTimeoutOrNull(5_000L) { deferred.await() }
            if (outcome == null) {
                bleCommands.timeout(generation, deferred)
                return BadgeCommandOutcome.TimedOut
            }
            return outcome
        } catch (cancelled: CancellationException) {
            if (writeAttempted) {
                bleCommands.cancelAfterAttempt(generation, deferred)
            } else {
                bleCommands.clearExact(deferred)
            }
            throw cancelled
        } finally {
            if (operationClaimed && !writeAttempted) {
                bleGattOperations.complete(BadgeBleGattOperation.CONTROL_WRITE)
            }
            bleMutationInFlight = false
            bleCommands.clearExact(deferred)
            if (transportGate.isCurrent(token)) readBleStatus(token)
        }
    }

    private suspend fun executeApHttpOnce(
        command: BadgeCommand,
        token: BadgeActiveTransportToken,
    ): BadgeCommandOutcome {
        if (token != activeHttpToken || !transportGate.isCurrent(token) ||
            token.transport != BadgeTransport.LOCAL_AP_HTTP
        ) {
            return BadgeCommandOutcome.Unsupported("Badge AP session changed")
        }
        val baseUrl = BADGE_AP_ENDPOINT.toHttpUrlOrNullSafe()
            ?: return BadgeCommandOutcome.Failed("Badge AP endpoint is invalid")
        val sentAt = clock.nowElapsedMs()
        val result = postJsonCommand(
            baseUrl = baseUrl,
            command = command,
            expectedAuthority = BadgeHttpCommandAuthority(token, BADGE_AP_ENDPOINT),
        )
        val outcome = result.fold(
            onSuccess = { response -> parseHttpCommandOutcome(response.code, response.body) },
            onFailure = { BadgeCommandOutcome.Failed("Badge AP command failed") },
        )
        val deadlineOutcome = enforceAckDeadline(outcome, elapsedAge(sentAt, clock.nowElapsedMs()))
        if (deadlineOutcome is BadgeCommandOutcome.Acknowledged && transportGate.isCurrent(token)) {
            fetchApStatus(transportGate.captureSession(sessionGeneration), showErrors = false)
        }
        return deadlineOutcome
    }

    private suspend fun executeDebugBridgeOnce(
        command: BadgeCommand,
        token: BadgeActiveTransportToken,
    ): BadgeCommandOutcome {
        if (command == BadgeCommand.Reboot || command == BadgeCommand.EnterBootloader) {
            return BadgeCommandOutcome.Unsupported("Recovery requires direct USB-C")
        }
        if (!debugBridgeConfig.enabled) {
            return BadgeCommandOutcome.Unsupported("Debug bridge is disabled in this build")
        }
        if (token != activeHttpToken || !transportGate.isCurrent(token) ||
            token.transport != BadgeTransport.DEBUG_BRIDGE
        ) {
            return BadgeCommandOutcome.Unsupported("Debug bridge session changed")
        }
        val baseUrl = debugBridgeConfig.baseUrl
            ?: return BadgeCommandOutcome.Unsupported("Debug bridge is disabled in this build")

        val pre = requestHttpStatus(baseUrl).getOrNull()
            ?: return BadgeCommandOutcome.Failed("Fresh physical badge status is required")
        val effectivePreStatus = pre.status.effectiveDebugReceipt()
        val preEvidence = effectivePreStatus.debugBridge
        val preIsLive = verifiedBadgeConnectionPhase(
            transport = BadgeTransport.DEBUG_BRIDGE,
            effectiveReceivedAtElapsedMs = effectivePreStatus.receivedAtElapsedMs,
            nowElapsedMs = clock.nowElapsedMs(),
        ) == BadgeConnectionPhase.LIVE
        if (!pre.isUsableDebugEvidence() || preEvidence == null || !preIsLive ||
            preEvidence.physicalSerialPort != state.value.connection.targetId
        ) {
            return BadgeCommandOutcome.Failed("Debug bridge physical status is incomplete")
        }
        val expectedTargetId = preEvidence.physicalSerialPort
            ?: return BadgeCommandOutcome.Failed("Debug bridge physical status is incomplete")
        if (!transportGate.isCurrent(token)) {
            return BadgeCommandOutcome.Unsupported("Debug bridge session changed")
        }

        val sentAt = clock.nowElapsedMs()
        val response = postJsonCommand(
            baseUrl = baseUrl,
            command = command,
            expectedAuthority = BadgeHttpCommandAuthority(token, expectedTargetId),
        ).getOrElse {
            return BadgeCommandOutcome.Failed("Debug bridge command failed")
        }
        if (response.code !in 200..299) {
            return BadgeCommandOutcome.Failed("Debug bridge command failed (${response.code})")
        }
        val acknowledged = parseUsbCommandLine(command, response.body)
            ?: BadgeCommandOutcome.Failed("Debug bridge returned no matching badge acknowledgement")
        val deadlineOutcome = enforceAckDeadline(
            acknowledged,
            elapsedAge(sentAt, clock.nowElapsedMs()),
        )
        if (deadlineOutcome !is BadgeCommandOutcome.Acknowledged) return deadlineOutcome

        val post = requestHttpStatus(baseUrl).getOrNull()
            ?: return BadgeCommandOutcome.Failed("Post-command physical status was unavailable")
        val postEvidence = post.status.debugBridge
        if (!post.isUsableDebugEvidence() || postEvidence == null ||
            !verifiesDebugPostCommandStatus(
                preSerialPort = preEvidence.physicalSerialPort,
                prePhysicalAtElapsedMs = preEvidence.physicalResponseAtElapsedMs,
                sentAtElapsedMs = sentAt,
                postSerialPort = postEvidence.physicalSerialPort,
                postPhysicalAtElapsedMs = postEvidence.physicalResponseAtElapsedMs,
                postAndroidReceiptAtElapsedMs = post.androidReceiptElapsedMs,
                postLastError = postEvidence.lastError,
            )
        ) {
            return BadgeCommandOutcome.Failed("Post-command physical status did not verify the same badge")
        }
        if (!transportGate.isCurrent(token)) {
            return BadgeCommandOutcome.Unsupported("Debug bridge session changed")
        }
        publishStatus(token, post.status.effectiveDebugReceipt())
        return deadlineOutcome
    }

    private fun startApPoller(session: BadgeTransportSessionToken) {
        if (apPollJob?.isActive == true) return
        apPollJob = scope.launch {
            while (isActive && transportGate.isSessionCurrent(session.sessionGeneration)) {
                if (activeUsbToken == null && activeBleToken == null) {
                    fetchApStatus(session, showErrors = false)
                }
                delay(AP_POLL_INTERVAL_MS)
            }
        }
    }

    private fun startDebugBridgePoller(session: BadgeTransportSessionToken) {
        if (!debugBridgeConfig.enabled || debugBridgePollJob?.isActive == true) return
        debugBridgePollJob = scope.launch {
            while (isActive && transportGate.isSessionCurrent(session.sessionGeneration)) {
                if (activeUsbToken == null && activeBleToken == null &&
                    activeHttpToken?.transport != BadgeTransport.LOCAL_AP_HTTP
                ) {
                    fetchDebugBridgeStatus(session, showErrors = false)
                }
                delay(DEBUG_BRIDGE_POLL_INTERVAL_MS)
            }
        }
    }

    private fun startBlePoller(session: BadgeTransportSessionToken) {
        if (blePollJob?.isActive == true) return
        blePollJob = scope.launch {
            while (isActive && transportGate.isSessionCurrent(session.sessionGeneration)) {
                if (activeUsbToken == null && activeBleToken == null &&
                    activeHttpToken?.transport != BadgeTransport.LOCAL_AP_HTTP
                ) {
                    startBleScanIfPossible(session)
                } else {
                    activeBleToken?.let { readBleStatus(it) }
                }
                delay(BLE_SCAN_INTERVAL_MS)
            }
        }
    }

    private fun startUsbStatusPoller(
        token: BadgeActiveTransportToken,
        connection: android.hardware.usb.UsbDeviceConnection,
    ) {
        usbStatusPollJob?.cancel()
        usbStatusPollJob = scope.launch {
            while (isActive && transportGate.isCurrent(token) && activeConnection === connection) {
                writeLine(token, "FOF_STATUS")
                delay(USB_STATUS_POLL_INTERVAL_MS)
            }
        }
    }

    private fun refreshUsb(session: BadgeTransportSessionToken) {
        if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            if (activeTransportToken == null) {
                _legacyState.update {
                    it.copy(
                        status = BadgeUsbStatus.DISCONNECTED,
                        deviceName = null,
                        message = "Connect USB-C or join the FoF badge AP",
                        transportLabel = "",
                    )
                }
            }
            return
        }
        if (candidates.size > 1) {
            reportAmbiguousBadgeDevices(candidates, session)
            return
        }
        val device = candidates.first()
        if (!usbManager.hasPermission(device)) {
            publishPermissionNeeded(device, session)
            return
        }
        scope.launch { connectToDevice(device, session) }
    }

    private fun publishPermissionNeeded(
        device: UsbDevice,
        session: BadgeTransportSessionToken,
    ) {
        transportGate.runIfSessionCurrent(session.sessionGeneration) {
            val evidence = BadgeConnectionEvidence(
                transport = BadgeTransport.USB_SERIAL,
                phase = BadgeConnectionPhase.PERMISSION_NEEDED,
                targetId = device.usbTargetId(),
                usbCandidateCount = findBadgeCandidates().size,
                exactEspressifVendorMatch = device.vendorId == ESPRESSIF_VENDOR_ID,
                releaseCertifiedMutations = certification.forTransport(BadgeTransport.USB_SERIAL),
            )
            stateStore.publishConnection(evidence)
            _legacyState.update {
                it.copy(
                    status = BadgeUsbStatus.PERMISSION_NEEDED,
                    deviceName = device.displayName(),
                    message = "FoF badge found. Tap Connect to grant USB access.",
                    transportLabel = "USB-C",
                    controlStatus = null,
                    detections = emptyList(),
                )
            }
        }
    }

    private suspend fun connectToDevice(
        device: UsbDevice,
        session: BadgeTransportSessionToken,
    ) {
        if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
        connectionMutex.withLock {
            if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
            val token = transportGate.claimNewConnection(
                session,
                BadgeTransport.USB_SERIAL,
            ) ?: return
            val targetId = device.usbTargetId()
            if (!transportGate.runIfCurrent(token) {
                    activeTransportToken = token
                    activeUsbToken = token
                    activeUsbTargetId = targetId
                    activeHttpToken = null
                    activeBleToken?.let { stale ->
                        if (stale != token) closeBleResources()
                    }
                    activeBleToken = null
                    activeBleCommandGeneration = bleCommands.invalidateTransport(
                        "Direct USB-C became active",
                    )
                    disconnectUsbResourcesOnly()
                    activeUsbCommandGeneration = usbCommands.resetTransportGeneration()
                }
            ) {
                return
            }

            val port = findReadablePort(device)
            if (port == null) {
                transportGate.runIfCurrent(token) {
                    publishUsbError(device, token, "No readable USB serial endpoint found")
                    activeTransportToken = null
                    activeUsbToken = null
                    activeUsbTargetId = null
                    transportGate.release(token)
                }
                return
            }
            val connection = usbManager.openDevice(device)
            if (connection == null || !connection.claimInterface(port.usbInterface, true)) {
                runCatching { connection?.close() }
                transportGate.runIfCurrent(token) {
                    publishUsbError(device, token, "Could not open USB badge")
                    activeTransportToken = null
                    activeUsbToken = null
                    activeUsbTargetId = null
                    transportGate.release(token)
                }
                return
            }
            val evidence = BadgeConnectionEvidence(
                transport = BadgeTransport.USB_SERIAL,
                transportGeneration = token.transportGeneration,
                phase = BadgeConnectionPhase.TRANSPORT_OPEN,
                targetId = targetId,
                usbCandidateCount = findBadgeCandidates().size,
                exactEspressifVendorMatch = device.vendorId == ESPRESSIF_VENDOR_ID,
                serialInterfaceReadable = true,
                releaseCertifiedMutations = certification.forTransport(BadgeTransport.USB_SERIAL),
            )
            if (!transportGate.runIfCurrent(token) {
                    activeConnection = connection
                    activeInterface = port.usbInterface
                    activeOutEndpoint = port.outEndpoint
                    stateStore.publishConnection(evidence)
                    _legacyState.update {
                        it.copy(
                            status = BadgeUsbStatus.CONNECTING,
                            deviceName = device.displayName(),
                            message = "USB serial open; verifying badge status",
                            transportLabel = "USB-C",
                            controlStatus = null,
                            detections = emptyList(),
                        )
                    }
                }
            ) {
                runCatching { connection.releaseInterface(port.usbInterface) }
                runCatching { connection.close() }
                return
            }
            startReader(connection, port.inEndpoint, token, activeUsbCommandGeneration)
            writeLine(token, "FOF_PING")
            writeLine(token, "FOF_STATUS")
            startUsbStatusPoller(token, connection)
        }
    }

    private fun startReader(
        connection: android.hardware.usb.UsbDeviceConnection,
        inEndpoint: UsbEndpoint,
        token: BadgeActiveTransportToken,
        commandGeneration: Long,
    ) {
        readJob?.cancel()
        readJob = scope.launch(Dispatchers.IO) {
            val buffer = ByteArray(256)
            val lineBuffer = StringBuilder()
            try {
                while (isActive && transportGate.isCurrent(token) && activeConnection === connection) {
                    val read = connection.bulkTransfer(
                        inEndpoint,
                        buffer,
                        buffer.size,
                        READ_TIMEOUT_MS,
                    )
                    if (read > 0) {
                        for (index in 0 until read) {
                            val character = buffer[index].toInt().toChar()
                            if (character == '\n' || character == '\r') {
                                if (lineBuffer.isNotEmpty()) {
                                    handleUsbLine(
                                        token,
                                        commandGeneration,
                                        lineBuffer.toString(),
                                    )
                                    lineBuffer.clear()
                                }
                            } else if (lineBuffer.length < MAX_LINE_CHARS) {
                                lineBuffer.append(character)
                            } else {
                                Log.w(TAG, "Dropping overlong badge line")
                                lineBuffer.clear()
                            }
                        }
                    } else {
                        delay(25)
                    }
                }
            } catch (error: Exception) {
                Log.w(TAG, "Badge USB reader stopped", error)
                disconnectUsb("Badge USB read failed", token)
            }
        }
    }

    private fun handleUsbLine(
        token: BadgeActiveTransportToken,
        commandGeneration: Long,
        rawLine: String,
    ) {
        transportGate.runIfCurrent(token) {
            if (commandGeneration != activeUsbCommandGeneration || token != activeUsbToken) {
                return@runIfCurrent
            }
            usbCommands.acceptSerialLine(commandGeneration, rawLine)
            val line = rawLine.trim()
            if (line.isEmpty()) return@runIfCurrent
            val detection = if (line.startsWith("FOF_DET:")) {
                parseDetection(line.removePrefix("FOF_DET:"))
            } else {
                null
            }
            val status = if (line.startsWith("FOF_STATUS:")) {
                val receipt = captureReceipt()
                parseBadgeControlStatus(
                    json = line.removePrefix("FOF_STATUS:"),
                    receivedAtElapsedMs = receipt.elapsedMs,
                    receivedAtWallClock = receipt.wallClock,
                )
            } else {
                null
            }
            if (status != null) publishStatus(token, status)
            if (detection != null) {
                stateStore.publishDetection(detection, MAX_RECENT_DETECTIONS)
            }
            _legacyState.update { current ->
                current.copy(
                    lastLine = line.take(160),
                    eventCount = if (detection != null) current.eventCount + 1 else current.eventCount,
                    message = when {
                        status != null -> "Badge status updated"
                        rawLine == "FOF_REBOOT:OK" -> "Badge reboot acknowledged; reconnect after restart"
                        rawLine == "FOF_BOOTLOADER:OK" -> "Badge bootloader acknowledged; reconnect after recovery"
                        line.startsWith("FOF_PONG:") -> "Badge replied ${line.removePrefix("FOF_PONG:")}"
                        line.startsWith("FOF_CTL_ERROR:") -> "Badge command failed"
                        line.startsWith("FOF_CTL_OK:") -> "Badge command acknowledged"
                        detection != null -> "Receiving badge events"
                        else -> current.message
                    },
                )
            }
        }
    }

    private suspend fun writeLine(token: BadgeActiveTransportToken, line: String): Boolean =
        writeUsbLine(token, line) == UsbLineWriteResult.WRITTEN

    private suspend fun writeUsbLine(
        token: BadgeActiveTransportToken,
        line: String,
        onAttempt: () -> Unit = {},
    ): UsbLineWriteResult =
        withContext(Dispatchers.IO) {
            val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
            var result = UsbLineWriteResult.NOT_ATTEMPTED
            transportGate.runIfCurrent(token) {
                if (token != activeUsbToken) return@runIfCurrent
                val connection = activeConnection ?: return@runIfCurrent
                val endpoint = activeOutEndpoint ?: return@runIfCurrent
                onAttempt()
                result = if (connection.bulkTransfer(
                        endpoint,
                        bytes,
                        bytes.size,
                        WRITE_TIMEOUT_MS,
                    ) == bytes.size
                ) {
                    UsbLineWriteResult.WRITTEN
                } else {
                    UsbLineWriteResult.ATTEMPTED_UNCERTAIN
                }
            }
            result
        }

    private suspend fun fetchNetworkStatus(
        session: BadgeTransportSessionToken,
        showErrors: Boolean,
    ): Boolean {
        if (!transportGate.isSessionCurrent(session.sessionGeneration)) return false
        if (fetchApStatus(session, showErrors = false)) return true
        if (debugBridgeConfig.enabled && fetchDebugBridgeStatus(session, showErrors = false)) return true
        if (hasBlePermissions()) startBleScanIfPossible(session)
        if (showErrors) {
            _legacyState.update { it.copy(message = "Badge USB-C, AP, or BLE not reachable") }
        }
        return false
    }

    private suspend fun fetchApStatus(
        session: BadgeTransportSessionToken,
        showErrors: Boolean,
    ): Boolean {
        val url = BADGE_AP_ENDPOINT.toHttpUrlOrNullSafe() ?: return false
        return fetchHttpStatus(
            session = session,
            transport = BadgeTransport.LOCAL_AP_HTTP,
            baseUrl = url,
            targetId = BADGE_AP_ENDPOINT,
            showErrors = showErrors,
        )
    }

    private suspend fun fetchDebugBridgeStatus(
        session: BadgeTransportSessionToken,
        showErrors: Boolean,
    ): Boolean {
        if (!debugBridgeConfig.enabled) return false
        val url = debugBridgeConfig.baseUrl ?: return false
        return fetchHttpStatus(
            session = session,
            transport = BadgeTransport.DEBUG_BRIDGE,
            baseUrl = url,
            targetId = null,
            showErrors = showErrors,
        )
    }

    private suspend fun fetchHttpStatus(
        session: BadgeTransportSessionToken,
        transport: BadgeTransport,
        baseUrl: HttpUrl,
        targetId: String?,
        showErrors: Boolean,
    ): Boolean {
        if (!transportGate.isSessionCurrent(session.sessionGeneration)) return false
        val statusRequest = httpStatusRequests.begin(
            sessionGeneration = session.sessionGeneration,
            transport = transport,
        ) ?: return false
        try {
            val token = transportGate.claim(session, transport) ?: return false
            val snapshot = requestHttpStatus(baseUrl).getOrNull()
            if (snapshot == null) {
                httpStatusRequests.runIfLatest(statusRequest) {
                    transportGate.runIfCurrent(token) {
                        val publishedLease = activeHttpToken == token &&
                            state.value.connection.transport == transport
                        if (showErrors) {
                            _legacyState.update {
                                it.copy(message = "${transport.label()} status unavailable")
                            }
                        }
                        if (!publishedLease ||
                            state.value.connection.phase == BadgeConnectionPhase.EXPIRED
                        ) {
                            if (activeHttpToken == token) activeHttpToken = null
                            if (activeTransportToken == token) activeTransportToken = null
                            transportGate.release(token)
                        }
                    }
                }
                return false
            }
            val status = if (transport == BadgeTransport.DEBUG_BRIDGE) {
                snapshot.status.effectiveDebugReceipt()
            } else {
                snapshot.status
            }
            val debug = status.debugBridge
            val evidenceComplete = transport != BadgeTransport.DEBUG_BRIDGE ||
                (debug?.physicalResponseAtElapsedMs != null &&
                    !debug.physicalSerialPort.isNullOrBlank() &&
                    debug.lastError != null)
            val evidence = BadgeConnectionEvidence(
                transport = transport,
                phase = if (evidenceComplete) {
                    verifiedBadgeConnectionPhase(
                        transport = transport,
                        effectiveReceivedAtElapsedMs = status.receivedAtElapsedMs,
                        nowElapsedMs = clock.nowElapsedMs(),
                    )
                } else {
                    BadgeConnectionPhase.TRANSPORT_OPEN
                },
                lastValidStatusAtElapsedMs = status.receivedAtElapsedMs.takeIf { evidenceComplete },
                protocolVersion = status.version.takeIf { evidenceComplete },
                targetId = when (transport) {
                    BadgeTransport.LOCAL_AP_HTTP -> targetId
                    BadgeTransport.DEBUG_BRIDGE -> debug?.physicalSerialPort
                    else -> null
                },
                badgeApEndpoint = targetId.takeIf { transport == BadgeTransport.LOCAL_AP_HTTP },
                debugBridgeSerialPort = debug?.physicalSerialPort,
                debugPhysicalStatusAtElapsedMs = debug?.physicalResponseAtElapsedMs,
                debugBridgeLastError = debug?.lastError,
                releaseCertifiedMutations = certification.forTransport(transport),
            )
            val statusIsCurrent = shouldRetainHttpLease(evidenceComplete, evidence.phase)
            var published = false
            httpStatusRequests.runIfLatest(statusRequest) {
                val publish: (BadgeActiveTransportToken) -> Unit = { publicationToken ->
                    val publicationEvidence = evidence.copy(
                        transportGeneration = publicationToken.transportGeneration,
                    )
                    activeTransportToken = publicationToken
                    activeHttpToken = publicationToken
                    stateStore.update { current ->
                        val changedTarget =
                            current.connection.transport != publicationEvidence.transport ||
                                current.connection.targetId != publicationEvidence.targetId
                        current.copy(
                            connection = publicationEvidence,
                            controlStatus = status.takeIf { statusIsCurrent },
                            detections = current.detections.takeUnless {
                                changedTarget || !statusIsCurrent
                            }.orEmpty(),
                        )
                    }
                    _legacyState.update {
                        it.copy(
                            status = publicationEvidence.toLegacyStatus(),
                            deviceName = when (transport) {
                                BadgeTransport.LOCAL_AP_HTTP -> "FoF Badge AP"
                                BadgeTransport.DEBUG_BRIDGE -> "FoF Debug Bridge"
                                else -> it.deviceName
                            },
                            message = if (statusIsCurrent) {
                                "${transport.label()} status verified"
                            } else if (publicationEvidence.phase == BadgeConnectionPhase.EXPIRED) {
                                "${transport.label()} physical status expired"
                            } else {
                                "${transport.label()} open; physical badge status unverified"
                            },
                            transportLabel = transport.label(),
                            controlStatus = status.takeIf { statusIsCurrent },
                            detections = it.detections.takeIf { statusIsCurrent }.orEmpty(),
                        )
                    }
                    if (!statusIsCurrent) {
                        activeHttpToken = null
                        if (activeTransportToken == publicationToken) activeTransportToken = null
                        transportGate.release(publicationToken)
                    }
                    published = true
                }

                val currentConnection = state.value.connection
                if (transport == BadgeTransport.DEBUG_BRIDGE &&
                    currentConnection.transport == BadgeTransport.DEBUG_BRIDGE &&
                    currentConnection.targetId != evidence.targetId
                ) {
                    transportGate.replaceAndRunIfCurrent(token, publish)
                } else {
                    transportGate.runIfCurrent(token) { publish(token) }
                }
            }
            return published && statusIsCurrent
        } finally {
            httpStatusRequests.finish(statusRequest)
        }
    }

    private suspend fun requestHttpStatus(baseUrl: HttpUrl): Result<HttpStatusSnapshot> =
        withContext(Dispatchers.IO) {
            runCatching {
                val request = Request.Builder()
                    .url(baseUrl.resolve("api/badge/status") ?: error("Invalid badge status URL"))
                    .get()
                    .build()
                val response = executeBadgeStatusCall(httpClients, request)
                check(response.code in 200..299) { "Badge status HTTP ${response.code}" }
                val receipt = captureReceipt()
                val status = parseBadgeControlStatus(
                    json = response.body,
                    receivedAtElapsedMs = receipt.elapsedMs,
                    receivedAtWallClock = receipt.wallClock,
                ) ?: error("Badge status was malformed")
                HttpStatusSnapshot(
                    status = status,
                    androidReceiptElapsedMs = receipt.elapsedMs,
                )
            }
        }

    private suspend fun postJsonCommand(
        baseUrl: HttpUrl,
        command: BadgeCommand,
        expectedAuthority: BadgeHttpCommandAuthority,
    ): Result<BadgeHttpResponse> {
        val request = runCatching {
            Request.Builder()
                .url(baseUrl.resolve("api/badge/control") ?: error("Invalid badge control URL"))
                .post(command.toControlJson().toString().toRequestBody(jsonMediaType))
                .build()
        }.getOrElse { return Result.failure(it) }
        val call = httpClients.command.newCall(request)
        return suspendCancellableCoroutine { continuation ->
            val completionClaimed = AtomicBoolean(false)
            fun complete(result: Result<BadgeHttpResponse>) {
                if (completionClaimed.compareAndSet(false, true)) {
                    continuation.resume(result)
                }
            }

            continuation.invokeOnCancellation {
                completionClaimed.compareAndSet(false, true)
                call.cancel()
            }
            val callback = object : Callback {
                override fun onFailure(call: Call, e: IOException) {
                    complete(Result.failure(e))
                }

                override fun onResponse(call: Call, response: Response) {
                    val result = runCatching {
                        response.use {
                            BadgeHttpResponse(it.code, it.body?.string().orEmpty())
                        }
                    }
                    complete(result)
                }
            }
            val startResult = runCatching {
                var commandStarted = false
                val noStatusRequestInFlight = httpStatusRequests.runIfNoActiveRequest(
                    expectedAuthority.token.transport,
                ) {
                    commandStarted = httpCommandStarts.startIfAuthorized(
                        expected = expectedAuthority,
                        current = ::currentHttpCommandAuthority,
                        start = { call.enqueue(callback) },
                    )
                }
                noStatusRequestInFlight && commandStarted
            }
            when {
                startResult.isFailure -> complete(Result.failure(startResult.exceptionOrNull()!!))
                !startResult.getOrDefault(false) -> complete(
                    Result.failure(IllegalStateException("Badge HTTP command authority changed")),
                )
            }
        }
    }

    private fun currentHttpCommandAuthority(): BadgeHttpCommandAuthority? {
        val token = activeHttpToken ?: return null
        val connection = state.value.connection
        val targetId = connection.targetId ?: return null
        if (!connection.matchesActiveToken(token)) return null
        return BadgeHttpCommandAuthority(token, targetId)
    }

    private fun publishStatus(token: BadgeActiveTransportToken, rawStatus: BadgeControlStatus) {
        val status = if (token.transport == BadgeTransport.DEBUG_BRIDGE) {
            rawStatus.effectiveDebugReceipt()
        } else {
            rawStatus
        }
        transportGate.runIfCurrent(token) {
            val current = state.value.connection
            if (!current.matchesActiveToken(token) ||
                current.targetId != token.targetIdentity(status)
            ) {
                return@runIfCurrent
            }
            val evidence = current.copy(
                phase = verifiedBadgeConnectionPhase(
                    transport = token.transport,
                    effectiveReceivedAtElapsedMs = status.receivedAtElapsedMs,
                    nowElapsedMs = clock.nowElapsedMs(),
                ),
                lastValidStatusAtElapsedMs = status.receivedAtElapsedMs,
                protocolVersion = status.version,
                negotiatedBleMtu = if (token.transport == BadgeTransport.BLE_GATT) {
                    negotiatedBleMtu
                } else {
                    current.negotiatedBleMtu
                },
                bleBonded = if (token.transport == BadgeTransport.BLE_GATT) {
                    activeGatt?.device?.bondState == BluetoothDevice.BOND_BONDED
                } else {
                    current.bleBonded
                },
                bleEncrypted = if (token.transport == BadgeTransport.BLE_GATT) {
                    status.bleControl.encrypted && status.bleControl.connected
                } else {
                    current.bleEncrypted
                },
                debugBridgeSerialPort = status.debugBridge?.physicalSerialPort
                    ?: current.debugBridgeSerialPort,
                debugPhysicalStatusAtElapsedMs = status.debugBridge?.physicalResponseAtElapsedMs
                    ?: current.debugPhysicalStatusAtElapsedMs,
                debugBridgeLastError = status.debugBridge?.lastError
                    ?: current.debugBridgeLastError,
                releaseCertifiedMutations = certification.forTransport(token.transport),
            )
            stateStore.update { it.copy(connection = evidence, controlStatus = status) }
            _legacyState.update {
                it.copy(
                    status = evidence.toLegacyStatus(),
                    message = "Badge status updated",
                    controlStatus = status,
                )
            }
        }
    }

    private fun hasBlePermissions(): Boolean {
        val adapter = bluetoothAdapter ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED &&
                ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED &&
                runCatching { adapter.isEnabled }.getOrDefault(false)
        } else {
            (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED ||
                ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_COARSE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED) &&
                runCatching { adapter.isEnabled }.getOrDefault(false)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startBleScanIfPossible(session: BadgeTransportSessionToken) {
        if (!transportGate.isSessionCurrent(session.sessionGeneration) ||
            !hasBlePermissions() || activeGatt != null
        ) {
            return
        }
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
                val name = runCatching { result.device.name }.getOrNull()
                    ?: result.scanRecord?.deviceName.orEmpty()
                val hasService = result.scanRecord?.serviceUuids
                    ?.any { it.uuid == BADGE_BLE_SERVICE_UUID } == true
                if (!hasService && !name.contains("FoF Badge", ignoreCase = true)) return
                val stopped = runCatching {
                    bleScanLeases.stopIfCurrent(this) { activeCallback ->
                        scanner.stopScan(activeCallback)
                    }
                }.getOrDefault(false)
                if (!stopped) return
                if (!transportGate.isSessionCurrent(session.sessionGeneration)) return
                connectBle(result.device, session)
            }

            override fun onScanFailed(errorCode: Int) {
                if (!bleScanLeases.completeIfCurrent(this)) return
                transportGate.runIfSessionCurrent(session.sessionGeneration) {
                    _legacyState.update { it.copy(message = "Badge BLE scan failed: $errorCode") }
                }
            }
        }
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid(BADGE_BLE_SERVICE_UUID)).build(),
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        var scanStarted = false
        runCatching {
            transportGate.runIfSessionHasNoActiveTransport(session.sessionGeneration) {
                if (!started.get() || activeGatt != null) return@runIfSessionHasNoActiveTransport
                scanStarted = bleScanLeases.startIfIdle(callback) {
                    scanner.startScan(filters, settings, callback)
                }
            }
        }.onFailure {
            bleScanLeases.completeIfCurrent(callback)
        }
        if (!scanStarted) return
        scope.launch {
            delay(BLE_SCAN_WINDOW_MS)
            runCatching {
                bleScanLeases.stopIfCurrent(callback) { expiredCallback ->
                    if (hasBlePermissions()) scanner.stopScan(expiredCallback)
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun connectBle(device: BluetoothDevice, session: BadgeTransportSessionToken) {
        if (!hasBlePermissions() || !transportGate.isSessionCurrent(session.sessionGeneration)) return
        val token = transportGate.claimNewConnection(session, BadgeTransport.BLE_GATT) ?: return
        val targetId = device.address
        if (!transportGate.runIfCurrent(token) {
                closeBleResources()
                activeTransportToken = token
                activeBleToken = token
                activeBleCommandGeneration = bleCommands.invalidateTransport(
                    "Badge BLE connection changed",
                )
                negotiatedBleMtu = 23
                stateStore.publishConnection(
                    BadgeConnectionEvidence(
                        transport = BadgeTransport.BLE_GATT,
                        transportGeneration = token.transportGeneration,
                        phase = BadgeConnectionPhase.CONNECTING,
                        targetId = targetId,
                        negotiatedBleMtu = 23,
                        bleBonded = device.bondState == BluetoothDevice.BOND_BONDED,
                        releaseCertifiedMutations = certification.forTransport(BadgeTransport.BLE_GATT),
                    ),
                )
                _legacyState.update {
                    it.copy(
                        status = BadgeUsbStatus.CONNECTING,
                        deviceName = runCatching { device.name }.getOrNull() ?: targetId,
                        message = "Connecting badge BLE",
                        transportLabel = "BLE",
                        controlStatus = null,
                        detections = emptyList(),
                    )
                }
            }
        ) {
            return
        }
        val gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, badgeGattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            @Suppress("DEPRECATION")
            device.connectGatt(context, false, badgeGattCallback)
        }
        if (!transportGate.runIfCurrent(token) {
                activeGatt = gatt
            }
        ) {
            runCatching { gatt.close() }
        }
    }

    private val badgeGattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            val token = activeBleToken ?: return
            if (gatt !== activeGatt || !transportGate.isCurrent(token)) return
            if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                closeBle("Badge BLE disconnected", token)
                return
            }
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                requestBleMtu(gatt, token)
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            val token = activeBleToken ?: return
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt ||
                    !bleGattOperations.complete(BadgeBleGattOperation.MTU_REQUEST)
                ) {
                    return@runIfCurrent
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    negotiatedBleMtu = mtu.coerceAtLeast(23)
                    stateStore.update { current ->
                        if (current.connection.transport == BadgeTransport.BLE_GATT) {
                            current.copy(
                                connection = current.connection.copy(
                                    negotiatedBleMtu = negotiatedBleMtu,
                                ),
                            )
                        } else {
                            current
                        }
                    }
                }
                discoverBleServices(gatt, token)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val token = activeBleToken ?: return
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt ||
                    !bleGattOperations.complete(BadgeBleGattOperation.SERVICE_DISCOVERY)
                ) {
                    return@runIfCurrent
                }
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    closeBle("Badge BLE service discovery failed", token)
                    return@runIfCurrent
                }
                val service: BluetoothGattService? = gatt.getService(BADGE_BLE_SERVICE_UUID)
                activeBleStatusChar = service?.getCharacteristic(BADGE_BLE_STATUS_UUID)
                activeBleControlChar = service?.getCharacteristic(BADGE_BLE_CONTROL_UUID)
                val current = state.value.connection
                stateStore.publishConnection(
                    current.copy(
                        phase = BadgeConnectionPhase.TRANSPORT_OPEN,
                        fofBleServicePresent = service != null,
                        bleStatusCharacteristicPresent = activeBleStatusChar != null,
                        bleControlCharacteristicPresent = activeBleControlChar != null,
                        negotiatedBleMtu = negotiatedBleMtu,
                        bleBonded = gatt.device.bondState == BluetoothDevice.BOND_BONDED,
                        releaseCertifiedMutations = certification.forTransport(BadgeTransport.BLE_GATT),
                    ),
                )
                if (activeBleStatusChar == null || activeBleControlChar == null) {
                    closeBle("Badge BLE service missing", token)
                    return@runIfCurrent
                }
                if (!enableBleStatusNotifications(gatt, activeBleStatusChar)) {
                    readBleStatus(token)
                }
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            val token = activeBleToken ?: return
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt || descriptor.uuid != CLIENT_CONFIG_UUID ||
                    !bleGattOperations.complete(BadgeBleGattOperation.DESCRIPTOR_WRITE)
                ) {
                    return@runIfCurrent
                }
                readBleStatus(token)
            }
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            val token = activeBleToken ?: return
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt || characteristic.uuid != BADGE_BLE_STATUS_UUID ||
                    !bleGattOperations.complete(BadgeBleGattOperation.STATUS_READ)
                ) {
                    return@runIfCurrent
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    @Suppress("DEPRECATION")
                    handleBleStatusBytes(gatt, token, characteristic.value ?: byteArrayOf())
                }
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            val token = activeBleToken ?: return
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt || characteristic.uuid != BADGE_BLE_STATUS_UUID ||
                    !bleGattOperations.complete(BadgeBleGattOperation.STATUS_READ)
                ) {
                    return@runIfCurrent
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    handleBleStatusBytes(gatt, token, value)
                }
            }
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            val token = activeBleToken ?: return
            if (gatt !== activeGatt || !transportGate.isCurrent(token)) return
            if (characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                @Suppress("DEPRECATION")
                handleBleStatusBytes(gatt, token, characteristic.value ?: byteArrayOf())
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            val token = activeBleToken ?: return
            if (gatt !== activeGatt || !transportGate.isCurrent(token)) return
            if (characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                handleBleStatusBytes(gatt, token, value)
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            val token = activeBleToken ?: return
            val generation = activeBleCommandGeneration
            transportGate.runIfCurrent(token) {
                if (gatt !== activeGatt || characteristic.uuid != BADGE_BLE_CONTROL_UUID) {
                    return@runIfCurrent
                }
                if (!bleGattOperations.complete(BadgeBleGattOperation.CONTROL_WRITE)) {
                    return@runIfCurrent
                }
                bleCommands.acceptWriteCallback(
                    generation,
                    success = status == BluetoothGatt.GATT_SUCCESS,
                )
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun requestBleMtu(gatt: BluetoothGatt, token: BadgeActiveTransportToken) {
        transportGate.runIfCurrent(token) {
            if (gatt !== activeGatt ||
                !bleGattOperations.tryBegin(BadgeBleGattOperation.MTU_REQUEST)
            ) {
                return@runIfCurrent
            }
            val started = runCatching { gatt.requestMtu(512) }.getOrDefault(false)
            if (!started) {
                bleGattOperations.complete(BadgeBleGattOperation.MTU_REQUEST)
                discoverBleServices(gatt, token)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun discoverBleServices(gatt: BluetoothGatt, token: BadgeActiveTransportToken) {
        transportGate.runIfCurrent(token) {
            if (gatt !== activeGatt ||
                !bleGattOperations.tryBegin(BadgeBleGattOperation.SERVICE_DISCOVERY)
            ) {
                return@runIfCurrent
            }
            if (!runCatching { gatt.discoverServices() }.getOrDefault(false)) {
                bleGattOperations.complete(BadgeBleGattOperation.SERVICE_DISCOVERY)
                closeBle("Badge BLE service discovery did not start", token)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableBleStatusNotifications(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic?,
    ): Boolean {
        if (characteristic == null || !hasBlePermissions()) return false
        if (!runCatching {
                gatt.setCharacteristicNotification(characteristic, true)
            }.getOrDefault(false)
        ) {
            return false
        }
        val descriptor = characteristic.getDescriptor(CLIENT_CONFIG_UUID) ?: return false
        if (!bleGattOperations.tryBegin(BadgeBleGattOperation.DESCRIPTOR_WRITE)) return false
        @Suppress("DEPRECATION")
        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(
                descriptor,
                BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE,
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            gatt.writeDescriptor(descriptor)
        }
        if (!started) bleGattOperations.complete(BadgeBleGattOperation.DESCRIPTOR_WRITE)
        return started
    }

    @SuppressLint("MissingPermission")
    private fun readBleStatus(token: BadgeActiveTransportToken) {
        transportGate.runIfCurrent(token) {
            if (bleMutationInFlight) return@runIfCurrent
            val gatt = activeGatt ?: return@runIfCurrent
            val characteristic = activeBleStatusChar ?: return@runIfCurrent
            if (!hasBlePermissions() ||
                !bleGattOperations.tryBegin(BadgeBleGattOperation.STATUS_READ)
            ) {
                return@runIfCurrent
            }
            if (!runCatching { gatt.readCharacteristic(characteristic) }.getOrDefault(false)) {
                bleGattOperations.complete(BadgeBleGattOperation.STATUS_READ)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeBleControl(
        token: BadgeActiveTransportToken,
        payload: JsonObject,
    ): Boolean {
        val bytes = payload.toString().toByteArray(Charsets.UTF_8)
        var started = false
        transportGate.runIfCurrent(token) {
            val gatt = activeGatt ?: return@runIfCurrent
            val characteristic = activeBleControlChar ?: return@runIfCurrent
            if (!hasBlePermissions()) return@runIfCurrent
            started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(
                    characteristic,
                    bytes,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                characteristic.value = bytes
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                gatt.writeCharacteristic(characteristic)
            }
        }
        return started
    }

    private fun handleBleStatusBytes(
        gatt: BluetoothGatt,
        token: BadgeActiveTransportToken,
        bytes: ByteArray,
    ) {
        transportGate.runIfCurrent(token) {
            if (gatt !== activeGatt) return@runIfCurrent
            val json = bytes.toString(Charsets.UTF_8).trim()
            if (json.isBlank()) return@runIfCurrent
            val receipt = captureReceipt()
            val status = parseBadgeControlStatus(
                json = json,
                receivedAtElapsedMs = receipt.elapsedMs,
                receivedAtWallClock = receipt.wallClock,
            ) ?: return@runIfCurrent
            publishStatus(token, status)
            _legacyState.update { it.copy(lastLine = json.take(160)) }
        }
    }

    private fun closeBle(reason: String, token: BadgeActiveTransportToken?) {
        if (token != null && token != activeBleToken) return
        val released = activeBleToken
        if (released == null) return
        transportGate.releaseAndRunIfCurrent(released) {
            activeBleToken = null
            if (activeTransportToken == released) activeTransportToken = null
            closeBleResources()
            activeBleCommandGeneration = bleCommands.invalidateTransport(reason)
            publishInactivePhase(BadgeConnectionPhase.DISCONNECTED, reason)
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeBleResources() {
        stopBleScan()
        val gatt = activeGatt
        activeGatt = null
        activeBleControlChar = null
        activeBleStatusChar = null
        negotiatedBleMtu = 23
        bleMutationInFlight = false
        bleGattOperations.reset()
        runCatching { if (hasBlePermissions()) gatt?.disconnect() }
        runCatching { gatt?.close() }
    }

    @SuppressLint("MissingPermission")
    private fun stopBleScan() {
        runCatching {
            bleScanLeases.stopCurrent { callback ->
                if (hasBlePermissions()) {
                    bluetoothAdapter?.bluetoothLeScanner?.stopScan(callback)
                }
            }
        }
    }

    private fun disconnectUsb(reason: String, token: BadgeActiveTransportToken?) {
        if (token != null && token != activeUsbToken) return
        val released = activeUsbToken
        if (released == null) return
        transportGate.releaseAndRunIfCurrent(released) {
            activeUsbToken = null
            activeUsbTargetId = null
            if (activeTransportToken == released) activeTransportToken = null
            activeUsbCommandGeneration = usbCommands.invalidateTransport(reason)
            publishInactivePhase(BadgeConnectionPhase.DISCONNECTED, reason)
            disconnectUsbResourcesOnly()
        }
    }

    private fun disconnectUsbResourcesOnly() {
        readJob?.cancel()
        readJob = null
        usbStatusPollJob?.cancel()
        usbStatusPollJob = null
        closeUsbResources()
    }

    private fun closeUsbResources() {
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

    private fun publishInactivePhase(phase: BadgeConnectionPhase, message: String) {
        stateStore.update { current ->
            current.copy(connection = current.connection.copy(phase = phase))
        }
        _legacyState.update {
            it.copy(status = phase.toLegacyStatus(), message = message, transportLabel = "")
        }
    }

    private fun publishUsbError(
        device: UsbDevice,
        token: BadgeActiveTransportToken,
        message: String,
    ) {
        val current = state.value.connection
        stateStore.publishConnection(
            current.copy(
                transport = BadgeTransport.USB_SERIAL,
                transportGeneration = token.transportGeneration,
                phase = BadgeConnectionPhase.ERROR,
                targetId = device.usbTargetId(),
                usbCandidateCount = findBadgeCandidates().size,
                exactEspressifVendorMatch = device.vendorId == ESPRESSIF_VENDOR_ID,
                releaseCertifiedMutations = certification.forTransport(BadgeTransport.USB_SERIAL),
            ),
        )
        _legacyState.update {
            it.copy(
                status = BadgeUsbStatus.ERROR,
                deviceName = device.displayName(),
                message = message,
                transportLabel = "USB-C",
            )
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

    private fun findBadgeCandidates(): List<UsbDevice> = usbManager.deviceList.values.filter { device ->
        device.vendorId == ESPRESSIF_VENDOR_ID ||
            device.safeManufacturerName().orEmpty().contains("Espressif", ignoreCase = true) ||
            device.safeProductName().orEmpty().contains("JTAG", ignoreCase = true)
    }

    private fun reportAmbiguousBadgeDevices(
        candidates: List<UsbDevice>,
        session: BadgeTransportSessionToken,
    ) {
        val names = candidates.take(3).joinToString(", ") { it.displayName() }
        transportGate.runIfSessionCurrent(session.sessionGeneration) {
            stateStore.publishConnection(
                BadgeConnectionEvidence(
                    transport = BadgeTransport.USB_SERIAL,
                    phase = BadgeConnectionPhase.ERROR,
                    usbCandidateCount = candidates.size,
                    releaseCertifiedMutations = certification.forTransport(BadgeTransport.USB_SERIAL),
                ),
            )
            _legacyState.update {
                it.copy(
                    status = BadgeUsbStatus.ERROR,
                    deviceName = null,
                    message = "Multiple Espressif USB devices found: $names. Connect only the badge.",
                    transportLabel = "USB-C",
                )
            }
        }
    }

    private fun findReadablePort(device: UsbDevice): UsbPort? {
        for (interfaceIndex in 0 until device.interfaceCount) {
            val usbInterface = device.getInterface(interfaceIndex)
            var inEndpoint: UsbEndpoint? = null
            var outEndpoint: UsbEndpoint? = null
            for (endpointIndex in 0 until usbInterface.endpointCount) {
                val endpoint = usbInterface.getEndpoint(endpointIndex)
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

    private fun parseDetection(json: String): BadgeUsbDetection? = runCatching {
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
            rssi = obj.get("rssi")?.asInt ?: 0,
        )
    }.getOrNull()

    private fun captureReceipt() = BadgeReceipt(
        elapsedMs = clock.nowElapsedMs(),
        wallClock = clock.nowWallClock(),
    )

    private fun BadgeControlStatus.effectiveDebugReceipt(): BadgeControlStatus {
        val physical = debugBridge?.physicalResponseAtElapsedMs ?: return this
        val effectiveElapsed = minOf(receivedAtElapsedMs, physical)
        val delta = elapsedAge(effectiveElapsed, receivedAtElapsedMs)
        return copy(
            receivedAtElapsedMs = effectiveElapsed,
            receivedAtWallClock = receivedAtWallClock.minusMillis(delta),
        )
    }

    private fun HttpStatusSnapshot.isUsableDebugEvidence(): Boolean {
        val evidence = status.debugBridge ?: return false
        return !evidence.physicalSerialPort.isNullOrBlank() &&
            evidence.physicalResponseAtElapsedMs != null &&
            evidence.lastError != null &&
            evidence.lastError.isEmpty()
    }

    private fun BadgeActiveTransportToken.targetIdentity(status: BadgeControlStatus): String? =
        when (transport) {
            BadgeTransport.USB_SERIAL,
            BadgeTransport.BLE_GATT,
            BadgeTransport.LOCAL_AP_HTTP,
            -> state.value.connection.targetId
            BadgeTransport.DEBUG_BRIDGE -> status.debugBridge?.physicalSerialPort
        }

    private fun BadgeConnectionEvidence.toLegacyStatus(): BadgeUsbStatus = when (phase) {
        BadgeConnectionPhase.DISCONNECTED,
        BadgeConnectionPhase.EXPIRED,
        -> BadgeUsbStatus.DISCONNECTED
        BadgeConnectionPhase.PERMISSION_NEEDED -> BadgeUsbStatus.PERMISSION_NEEDED
        BadgeConnectionPhase.CONNECTING,
        BadgeConnectionPhase.TRANSPORT_OPEN,
        -> BadgeUsbStatus.CONNECTING
        BadgeConnectionPhase.ERROR -> BadgeUsbStatus.ERROR
        BadgeConnectionPhase.LIVE,
        BadgeConnectionPhase.STALE,
        -> when (transport) {
            BadgeTransport.USB_SERIAL -> BadgeUsbStatus.CONNECTED
            BadgeTransport.LOCAL_AP_HTTP -> BadgeUsbStatus.AP_CONNECTED
            BadgeTransport.BLE_GATT -> BadgeUsbStatus.BLE_CONNECTED
            BadgeTransport.DEBUG_BRIDGE -> BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED
            null -> BadgeUsbStatus.DISCONNECTED
        }
    }

    private fun BadgeConnectionPhase.toLegacyStatus(): BadgeUsbStatus = when (this) {
        BadgeConnectionPhase.DISCONNECTED,
        BadgeConnectionPhase.EXPIRED,
        -> BadgeUsbStatus.DISCONNECTED
        BadgeConnectionPhase.PERMISSION_NEEDED -> BadgeUsbStatus.PERMISSION_NEEDED
        BadgeConnectionPhase.CONNECTING,
        BadgeConnectionPhase.TRANSPORT_OPEN,
        -> BadgeUsbStatus.CONNECTING
        BadgeConnectionPhase.LIVE,
        BadgeConnectionPhase.STALE,
        -> BadgeUsbStatus.CONNECTED
        BadgeConnectionPhase.ERROR -> BadgeUsbStatus.ERROR
    }

    private fun BadgeTransport.label(): String = when (this) {
        BadgeTransport.USB_SERIAL -> "USB-C"
        BadgeTransport.LOCAL_AP_HTTP -> "Badge AP"
        BadgeTransport.BLE_GATT -> "BLE"
        BadgeTransport.DEBUG_BRIDGE -> "Debug Bridge"
    }

    private fun BadgeCommandOutcome.userMessage(transportLabel: String): String = when (this) {
        is BadgeCommandOutcome.Acknowledged -> acknowledgement.message
        is BadgeCommandOutcome.Accepted -> message
        is BadgeCommandOutcome.Failed -> message
        is BadgeCommandOutcome.Unsupported -> reason
        BadgeCommandOutcome.TimedOut ->
            "${transportLabel.ifBlank { "Badge" }} command timed out; reconnect before retrying"
    }

    private fun UsbDevice.usbTargetId(): String =
        "usb:${vendorId.toString(16)}:${deviceId}:${deviceName}"

    private fun UsbDevice.displayName(): String {
        val parts = listOfNotNull(
            safeManufacturerName()?.takeIf { it.isNotBlank() },
            safeProductName()?.takeIf { it.isNotBlank() },
        )
        return parts.joinToString(" ").ifBlank { deviceName }
    }

    private fun UsbDevice.safeManufacturerName(): String? =
        runCatching { manufacturerName }.getOrNull()

    private fun UsbDevice.safeProductName(): String? =
        runCatching { productName }.getOrNull()

    private fun Intent.usbDeviceExtra(): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    private fun JsonObject.optString(key: String): String = runCatching {
        get(key)?.takeIf { !it.isJsonNull }?.asString.orEmpty()
    }.getOrDefault("")

    private fun JsonObject.optFloat(key: String): Float = runCatching {
        get(key)?.takeIf { !it.isJsonNull }?.asFloat ?: 0f
    }.getOrDefault(0f)

    private fun String.toHttpUrlOrNullSafe(): HttpUrl? =
        runCatching { HttpUrl.Builder().scheme("http").host(substringAfter("http://")).build() }
            .getOrNull()

    private data class UsbPort(
        val usbInterface: UsbInterface,
        val inEndpoint: UsbEndpoint,
        val outEndpoint: UsbEndpoint,
    )

    private data class BadgeReceipt(
        val elapsedMs: Long,
        val wallClock: Instant,
    )

    private data class HttpStatusSnapshot(
        val status: BadgeControlStatus,
        val androidReceiptElapsedMs: Long,
    )

}

private fun elapsedAge(startElapsedMs: Long, endElapsedMs: Long): Long = when {
    endElapsedMs <= startElapsedMs -> 0L
    else -> runCatching { Math.subtractExact(endElapsedMs, startElapsedMs) }
        .getOrDefault(Long.MAX_VALUE)
}
