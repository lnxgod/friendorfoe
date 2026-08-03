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
import android.net.Uri
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import com.friendorfoe.BuildConfig
import com.friendorfoe.detection.BleInvestigationChunk
import com.friendorfoe.detection.BleInvestigationChunkAssembler
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.elapsedRealtimeMs
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Call
import okhttp3.Callback
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.UUID
import java.util.concurrent.TimeUnit
import kotlin.coroutines.coroutineContext
import kotlin.coroutines.resume

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
    val rssi: Int,
    val receivedAtElapsedMs: Long = elapsedRealtimeMs(),
)

internal fun parseBadgeUsbDetection(
    json: String,
    receivedAtElapsedMs: Long,
): BadgeUsbDetection? = runCatching {
    val obj = JsonParser.parseString(json).asJsonObject
    BadgeUsbDetection(
        id = obj.get("id")?.asString.orEmpty(),
        manufacturer = obj.get("manufacturer")?.asString.orEmpty(),
        badgeLabel = runCatching {
            obj.get("badge_label")?.takeIf { !it.isJsonNull }?.asString.orEmpty()
        }.getOrDefault(""),
        badgeClass = runCatching {
            obj.get("badge_class")?.takeIf { !it.isJsonNull }?.asString.orEmpty()
        }.getOrDefault(""),
        badgeEntityKey = runCatching {
            obj.get("badge_entity_key")?.takeIf { !it.isJsonNull }?.asString.orEmpty()
        }.getOrDefault(""),
        source = obj.get("source")?.asInt ?: -1,
        confidence = obj.get("confidence")?.asFloat ?: 0f,
        threatScore = runCatching {
            obj.get("threat_score")?.takeIf { !it.isJsonNull }?.asFloat ?: 0f
        }.getOrDefault(0f),
        rssi = obj.get("rssi")?.asInt ?: 0,
        receivedAtElapsedMs = receivedAtElapsedMs,
    )
}.getOrNull()

data class BadgeDisplayState(
    val active: Boolean = false,
    val detailMode: Boolean = false,
    val detailPage: Int = 0,
    val focusIndex: Int = 0,
    val focusTotal: Int = 0,
    val itemIndex: Int = 0,
    val itemTotal: Int = 0,
    val lane: String = "",
    val title: String = "",
    val detail: String = "",
    val evidence: String = "",
    val entityKey: String = "",
    val displayId: String = "",
    val threatClass: String = "",
    val category: String = "",
    val code: String = "",
    val source: String = "",
    val ssid: String = "",
    val bssid: String = "",
    val authMode: Int = -1,
    val freqMhz: Int = 0,
    val score: Int = 0,
    val confidencePct: Int = 0,
    val evidenceQuality: Int = 0,
    val displayRank: Int = 0,
    val ageSeconds: Int = 0,
    val lastSeenSeconds: Int = 0,
    val rssi: Int = 0,
    val bestRssi: Int = 0,
    val events: Int = 0,
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
    val evidence: String = "",
    val threatClass: String,
    val category: String = "",
    val code: String = "",
    val displayId: String = "",
    val source: String = "",
    val sourceId: Int = 0,
    val ssid: String = "",
    val bssid: String = "",
    val authMode: Int = -1,
    val freqMhz: Int = 0,
    val score: Int,
    val confidencePct: Int = 0,
    val evidenceQuality: Int = 0,
    val displayRank: Int = 0,
    val ageSeconds: Int,
    val lastSeenSeconds: Int = 0,
    val snapshotAtElapsedMs: Long = -1L,
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

data class BadgeBleControlStatus(
    val enabled: Boolean = false,
    val bonded: Boolean = false,
    val pairingAgeSeconds: Long? = null,
    val pairingWindowSeconds: Int = 10,
    val connected: Boolean = false,
    val encrypted: Boolean = false,
    val lastError: String = "",
    val rx: Long = 0,
    val tx: Long = 0
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
    fun toJsonObject(): JsonObject {
        val normalized = normalizedV1()
        return JsonObject().apply {
            addProperty("version", normalized.version)
            add("classes", JsonObject().apply {
                BadgeDisplayPolicyClasses.forEach { info ->
                    val config = normalized.classes.getValue(info.key)
                    add(info.key, JsonObject().apply {
                        addProperty("enabled", config.enabled)
                        addProperty("lane", config.lane)
                        addProperty("min_proximity", config.minProximity)
                        addProperty("priority", config.priority)
                    })
                }
            })
        }
    }
}

fun BadgeDisplayPolicy.withClassEnabled(key: String, enabled: Boolean): BadgeDisplayPolicy {
    val defaults = defaultBadgeDisplayPolicyClasses()
    val current = classes[key] ?: defaults[key] ?: return this
    val next = if (enabled) {
        val fallback = defaults.getValue(key)
        current.copy(
            enabled = true,
            lane = if (current.lane == "off") fallback.lane else current.lane,
            minProximity = if (current.lane == "off") fallback.minProximity else current.minProximity,
            priority = if (current.lane == "off") fallback.priority else current.priority
        )
    } else {
        current.copy(enabled = false, lane = "off")
    }
    return copy(classes = classes + (key to next))
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
    BadgeDisplayPolicyClassInfo("scanner_status", "Scanner Status"),
    BadgeDisplayPolicyClassInfo("ble_attack", "BLE Attack")
)

fun defaultBadgeDisplayPolicyClasses(): Map<String, BadgeDisplayClassPolicy> = mapOf(
    "drone" to BadgeDisplayClassPolicy(true, "both", "present", 100),
    "meta" to BadgeDisplayClassPolicy(true, "both", "present", 95),
    "tracker" to BadgeDisplayClassPolicy(true, "lower", "near", 70),
    "wifi_attack" to BadgeDisplayClassPolicy(true, "both", "present", 90),
    "skimmer" to BadgeDisplayClassPolicy(false, "off", "close", 0),
    "camera" to BadgeDisplayClassPolicy(true, "lower", "near", 65),
    "flock" to BadgeDisplayClassPolicy(true, "both", "present", 85),
    "lock" to BadgeDisplayClassPolicy(true, "lower", "near", 55),
    "hid" to BadgeDisplayClassPolicy(true, "lower", "close", 45),
    "beacon" to BadgeDisplayClassPolicy(true, "lower", "near", 30),
    "event_badge" to BadgeDisplayClassPolicy(true, "lower", "near", 35),
    "auracast" to BadgeDisplayClassPolicy(true, "lower", "near", 20),
    "scanner_status" to BadgeDisplayClassPolicy(true, "lower", "present", 10),
    "ble_attack" to BadgeDisplayClassPolicy(true, "both", "present", 92)
)

fun defaultBadgeDisplayPolicy(): BadgeDisplayPolicy =
    BadgeDisplayPolicy(classes = defaultBadgeDisplayPolicyClasses())

fun BadgeDisplayPolicy.normalizedV1(): BadgeDisplayPolicy {
    val defaults = defaultBadgeDisplayPolicyClasses()
    val lanes = setOf("top", "lower", "both", "off")
    val proximities = setOf("present", "near", "close")
    return BadgeDisplayPolicy(
        version = 1,
        classes = BadgeDisplayPolicyClasses.associate { info ->
            val fallback = defaults.getValue(info.key)
            val supplied = classes[info.key] ?: fallback
            info.key to supplied.copy(
                lane = supplied.lane.takeIf { it in lanes } ?: fallback.lane,
                minProximity = supplied.minProximity
                    .takeIf { it in proximities } ?: fallback.minProximity,
                priority = supplied.priority.coerceIn(0, 100),
            )
        },
    )
}

fun badgeDisplayPolicyCommandJson(
    policy: BadgeDisplayPolicy,
    persist: Boolean = true
): JsonObject = JsonObject().apply {
    addProperty("cmd", "badge_display_policy")
    addProperty("persist", persist)
    add("policy", policy.toJsonObject())
}

enum class BadgeDisplayNavAction(val wireValue: String) {
    NEXT("next"),
    DETAIL("detail"),
    PAGE("page"),
    BACK("back"),
}

fun badgeDisplayNavCommandJson(action: BadgeDisplayNavAction): JsonObject = JsonObject().apply {
    addProperty("cmd", "display_nav")
    addProperty("action", action.wireValue)
}

data class BadgeTheme(
    val version: Int = 1,
    val palette: String = "field",
    val background: String = "dark",
    val brightness: Int = 100,
    val accents: Map<String, Int> = defaultBadgeThemeAccents()
) {
    fun toJsonObject(): JsonObject {
        val normalized = normalizedV1()
        return JsonObject().apply {
            addProperty("version", normalized.version)
            addProperty("palette", normalized.palette)
            addProperty("background", normalized.background)
            addProperty("brightness", normalized.brightness)
            add("accents", JsonObject().apply {
                BadgeThemeAccentClasses.forEach { accent ->
                    addProperty(accent.key, normalized.accents.getValue(accent.key))
                }
            })
        }
    }
}

data class BadgeThemeAccentInfo(
    val key: String,
    val label: String,
    val defaultRgb565: Int
)

val BadgeThemeAccentClasses = listOf(
    BadgeThemeAccentInfo("drone", "Drone", 0xFEA0),
    BadgeThemeAccentInfo("meta", "Meta", 0xF833),
    BadgeThemeAccentInfo("tracker", "Tracker", 0xF81F),
    BadgeThemeAccentInfo("flock", "Flock", 0xA81F),
    BadgeThemeAccentInfo("wifi_attack", "WiFi Attack", 0x07FF),
    BadgeThemeAccentInfo("clear", "Clear", 0x2F65)
)

val BadgeThemePalettes = listOf("field", "night", "neon", "mono")
val BadgeThemeBackgrounds = listOf("dark", "dim", "scanline")

fun defaultBadgeThemeAccents(): Map<String, Int> =
    BadgeThemeAccentClasses.associate { it.key to it.defaultRgb565 }

fun defaultBadgeTheme(): BadgeTheme = BadgeTheme()

fun badgeThemeCommandJson(theme: BadgeTheme, persist: Boolean = true): JsonObject =
    JsonObject().apply {
        addProperty("cmd", "badge_theme")
        addProperty("persist", persist)
        add("theme", theme.toJsonObject())
    }

private val BadgeSetModeValues = setOf(
    "off",
    "usb",
    "usb_only",
    "local_ap",
    "ap",
    "backend",
)

fun badgeSetModeCommandJson(mode: String): JsonObject? =
    mode.takeIf { it in BadgeSetModeValues }?.let { validatedMode ->
        JsonObject().apply {
            addProperty("cmd", "set_mode")
            addProperty("mode", validatedMode)
            addProperty("persist", true)
        }
    }

fun badgeRebootCommandJson(): JsonObject = JsonObject().apply {
    addProperty("cmd", "reboot")
}

fun badgeDisplayPolicyResetCommandJson(persist: Boolean = true): JsonObject =
    JsonObject().apply {
        addProperty("cmd", "badge_display_policy_reset")
        addProperty("persist", persist)
    }

fun badgeThemeResetCommandJson(persist: Boolean = true): JsonObject =
    JsonObject().apply {
        addProperty("cmd", "badge_theme_reset")
        addProperty("persist", persist)
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

data class BadgeUsbHealthStatus(
    val schema: Int = 0,
    val taskStarted: Boolean = false,
    val hostConnected: Boolean = false,
    val parserState: String = "",
    val rxBytes: Long = 0L,
    val validCommands: Long = 0L,
    val responsesCompleted: Long = 0L,
    val requiredResponseFailures: Long = 0L,
    val malformedLines: Long = 0L,
    val droppedProgressFrames: Long = 0L,
    val droppedOptionalFrames: Long = 0L,
    val uploadReceived: Long = 0L,
    val uploadSize: Long = 0L,
    val taskHeartbeatAgeSeconds: Long? = null,
    val lastRxAgeSeconds: Long? = null,
    val lastCommandAgeSeconds: Long? = null,
    val lastResponseAgeSeconds: Long? = null,
    val lastUploadProgressAgeSeconds: Long? = null,
)

private val BadgeUsbParserStates = setOf(
    "command",
    "scanner_upload",
    "uplink_upload",
)

data class BadgeControlStatus(
    val version: String = "",
    val firmwareTarget: String = "",
    val firmwareName: String = "",
    val appProject: String = "",
    val hardwareType: String = "",
    val hardwareId: String = "",
    val productFamily: String = "",
    val project: String = "",
    val hardware: String = "",
    val mac: String = "",
    val capabilities: Set<String> = emptySet(),
    val runningPartition: String = "",
    val pendingVerify: Boolean = false,
    val rollbackState: String = "",
    val lastExpectedRebootReason: String = "",
    val usbHealth: BadgeUsbHealthStatus = BadgeUsbHealthStatus(),
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
    val filteredCounts: Map<String, Int> = emptyMap(),
    val theme: BadgeTheme = defaultBadgeTheme(),
    val themeHash: Long = 0,
    val displayState: BadgeDisplayState? = null,
    val bleControl: BadgeBleControlStatus = BadgeBleControlStatus(),
    val safeMode: Boolean = false,
    val safeReason: String = "",
    val resetReason: String = "",
    val resetReasonCode: Long = 0,
    val resetExpected: Boolean = false,
    val crashCount: Int = 0,
    val recoveryMode: String = "",
    val usbControlAgeSeconds: Long? = null,
    val stackMainFree: Int = 0,
    val stackDisplayFree: Int = 0,
    val stackUsbFree: Int = 0,
    val stackUartBleFree: Int = 0,
    val stackUartWifiFree: Int = 0,
    val heapInternalFree: Long = 0,
    val heapInternalMinFree: Long = 0,
    val heapInternalLargest: Long = 0,
    val psramTotal: Long = 0,
    val psramFree: Long = 0,
    val psramLargest: Long = 0
)

data class BadgeUsbState(
    val status: BadgeUsbStatus = BadgeUsbStatus.DISCONNECTED,
    val deviceName: String? = null,
    val message: String = "Attach a FoF badge over USB-C",
    val transportLabel: String = "",
    val lastLine: String? = null,
    val eventCount: Int = 0,
    val detections: List<BadgeUsbDetection> = emptyList(),
    val activity: List<BadgeUsbActivity> = emptyList(),
    val controlStatus: BadgeControlStatus? = null,
    val firmwareProgress: BadgeFirmwareProgress? = null
)

internal fun parseBadgeControlStatus(
    json: String,
    snapshotAtElapsedMs: Long = elapsedRealtimeMs(),
): BadgeControlStatus? {
    return runCatching {
        val obj = JsonParser.parseString(json).asJsonObject
        val countsObj = runCatching { obj.getAsJsonObject("counts") }.getOrNull()
        val reportingObj = runCatching { obj.getAsJsonObject("reporting") }.getOrNull()
        val displayPolicy = parseBadgeDisplayPolicy(
            runCatching { obj.getAsJsonObject("display_policy") }.getOrNull()
        )
        val filteredCounts = parseBadgeIntMap(
            runCatching { obj.getAsJsonObject("filtered_counts") }.getOrNull()
        )
        val theme = parseBadgeTheme(
            runCatching { obj.getAsJsonObject("theme") }.getOrNull()
        )
        val displayState = parseBadgeDisplayState(
            runCatching { obj.getAsJsonObject("display_state") }.getOrNull()
        )
        val bleControl = parseBadgeBleControlStatus(
            runCatching { obj.getAsJsonObject("ble_control") }.getOrNull()
        )
        val usbHealth = parseBadgeUsbHealthStatus(
            runCatching { obj.getAsJsonObject("usb_health") }.getOrNull()
        )
        val entities = obj.getAsJsonArray("entities")?.mapNotNull { element ->
            runCatching {
                val e = element.asJsonObject
                BadgeThreatEntity(
                    label = e.badgeOptString("label"),
                    detail = e.badgeOptString("detail"),
                    evidence = e.badgeOptString("evidence"),
                    threatClass = e.badgeOptString("class"),
                    category = e.badgeOptString("category"),
                    code = e.badgeOptString("code"),
                    displayId = e.badgeOptString("display_id"),
                    source = e.badgeOptString("source"),
                    sourceId = e.badgeOptInt("source_id"),
                    ssid = e.badgeOptString("ssid"),
                    bssid = e.badgeOptString("bssid"),
                    authMode = e.badgeOptInt("auth_m", -1),
                    freqMhz = e.badgeOptInt("freq_mhz"),
                    score = e.badgeOptInt("score"),
                    confidencePct = e.badgeOptInt("confidence_pct"),
                    evidenceQuality = e.badgeOptInt("evidence_quality"),
                    displayRank = e.badgeOptInt("display_rank"),
                    ageSeconds = e.badgeOptInt("age_s"),
                    lastSeenSeconds = e.badgeOptInt("last_seen_s"),
                    snapshotAtElapsedMs = snapshotAtElapsedMs,
                    rssi = e.badgeOptInt("rssi"),
                    bestRssi = e.badgeOptInt("best_rssi"),
                    events = e.badgeOptInt("events"),
                    seenCount = e.badgeOptInt("seen_count"),
                    groupCount = e.badgeOptInt("group_count"),
                    proximityLevel = e.badgeOptInt("proximity_level"),
                    stale = e.badgeOptBoolean("stale"),
                    lat = e.badgeOptDoubleOrNull("lat"),
                    lon = e.badgeOptDoubleOrNull("lon"),
                    altitudeM = e.badgeOptFloatOrNull("altitude_m"),
                    operatorLat = e.badgeOptDoubleOrNull("operator_lat"),
                    operatorLon = e.badgeOptDoubleOrNull("operator_lon"),
                    operatorId = e.badgeOptString("operator_id").ifBlank { null }
                )
            }.getOrNull()
        }.orEmpty()
        val scannerArray = sequenceOf("scanners", "scanner", "scanner_summaries")
            .mapNotNull { key -> runCatching { obj.getAsJsonArray(key) }.getOrNull() }
            .firstOrNull()
        val scanners = scannerArray?.mapNotNull { element ->
            runCatching {
                val s = element.asJsonObject
                val healthObject = runCatching { s.getAsJsonObject("health") }.getOrNull()
                val connected = s.badgeOptBoolean("connected")
                val commandHealthy = healthObject?.badgeStrictBoolean("command")
                val radioHealthy = healthObject?.badgeStrictBoolean("radio")
                val roleAcked = s.badgeStrictBoolean("role_acked")
                    ?: healthObject?.badgeStrictBoolean("role_acked")
                    ?: false
                BadgeScannerStatus(
                    slot = s.badgeOptInt("slot", -1),
                    uart = s.badgeOptString("uart"),
                    connected = connected,
                    slotRole = s.badgeOptString("slot_role"),
                    expectedScanProfile = s.badgeOptString("expected_scan_profile"),
                    scanProfile = s.badgeOptString("scan_profile").ifBlank {
                        s.badgeStrictNonNegativeInt("profile")?.toString().orEmpty()
                    },
                    roleAcked = roleAcked,
                    health = s.badgeOptString("health").ifBlank {
                        when {
                            !connected -> "disconnected"
                            commandHealthy == true && radioHealthy == true && roleAcked -> "healthy"
                            commandHealthy != null || radioHealthy != null -> "degraded"
                            else -> ""
                        }
                    },
                    uartRawSeen = s.badgeOptBoolean("uart_raw_seen"),
                    uartRawAgeSeconds = s.badgeOptLongOrNull("uart_raw_age_s"),
                    uartJsonErrors = s.badgeOptInt("uart_json_err"),
                    commandRx = s.badgeOptInt("cmd_rx"),
                    commandLastAgeSeconds = s.badgeOptLongOrNull("cmd_last_age_s"),
                    bleAdvSeen = s.badgeOptInt("ble_adv_seen"),
                    bleFpEmit = s.badgeOptInt("ble_fp_emit"),
                    bleMetaSeen = s.badgeOptInt("ble_meta_seen"),
                    bleTrackerSeen = s.badgeOptInt("ble_tracker_seen"),
                    ridEmit = s.badgeOptInt("rid_emit"),
                    privacySeen = s.badgeOptInt("privacy_seen"),
                    wifiTotalFrames = s.badgeOptInt("wifi_total_frames"),
                    wifiDroneSsidEmit = s.badgeOptInt("wifi_drone_ssid_emit"),
                    wifiNotableSsidEmit = s.badgeOptInt("wifi_notable_ssid_emit"),
                    wifiLastDroneSsid = s.badgeOptString("wifi_last_drone_ssid"),
                    wifiLastNotableSsid = s.badgeOptString("wifi_last_notable_ssid"),
                    displayPolicyHash = s.badgeOptLong("display_policy_hash"),
                    displayPolicyAckHash = s.badgeOptLong("display_policy_ack_hash"),
                    filteredCounts = parseBadgeIntMap(
                        runCatching { s.getAsJsonObject("filtered_counts") }.getOrNull()
                    ),
                    firmwareState = s.badgeOptString("fw_state"),
                    targetVersion = s.badgeOptString("target_ver"),
                    otaState = s.badgeOptString("ota_state"),
                    lastFirmwareError = s.badgeOptString("last_fw_error")
                )
            }.getOrNull()
        }.orEmpty()

        BadgeControlStatus(
            version = obj.badgeOptString("version"),
            firmwareTarget = obj.badgeOptString("target"),
            firmwareName = obj.badgeOptString("firmware_name"),
            appProject = obj.badgeOptString("app_project"),
            hardwareType = obj.badgeOptString("hardware_type"),
            hardwareId = obj.badgeStrictString("hardware_id").orEmpty(),
            productFamily = obj.badgeStrictString("product_family").orEmpty(),
            project = obj.badgeStrictString("project").orEmpty(),
            hardware = obj.badgeStrictString("hardware").orEmpty(),
            mac = obj.badgeStrictString("mac").orEmpty(),
            capabilities = obj.badgeStrictStringSet("capabilities"),
            runningPartition = obj.badgeStrictString("running_partition").orEmpty(),
            pendingVerify = obj.badgeStrictBoolean("pending_verify") ?: false,
            rollbackState = obj.badgeStrictString("rollback_state").orEmpty(),
            lastExpectedRebootReason =
                obj.badgeStrictString("last_expected_reboot_reason").orEmpty(),
            usbHealth = usbHealth,
            mode = obj.badgeOptString("mode").ifBlank { "local_ap" },
            modeLabel = obj.badgeOptString("mode_label").ifBlank { "Local AP" },
            threatScore = obj.badgeOptFloat("threat_score"),
            colorRgb565 = obj.badgeOptInt("color_rgb565"),
            reporting = BadgeReportingStatus(
                networkMode = reportingObj?.badgeOptString("network_mode")
                    ?: obj.badgeOptString("network_mode").ifBlank { obj.badgeOptString("mode") },
                backendEnabled = reportingObj?.badgeOptBoolean("backend_enabled")
                    ?: obj.badgeOptBoolean("backend_enabled"),
                networkTtlSeconds = reportingObj?.badgeOptInt("network_ttl_s")
                    ?: obj.badgeOptInt("network_ttl_s"),
                wifiSta = reportingObj?.badgeOptBoolean("wifi_sta") ?: obj.badgeOptBoolean("wifi_sta"),
                standalone = reportingObj?.badgeOptBoolean("standalone") ?: false,
                uploadsOk = reportingObj?.badgeOptInt("uploads_ok") ?: 0,
                uploadsFail = reportingObj?.badgeOptInt("uploads_fail") ?: 0,
                lastUploadAgeSeconds = reportingObj?.badgeOptLongOrNull("last_upload_age_s")
            ),
            counts = BadgeThreatCounts(
                drone = countsObj?.badgeOptInt("drone") ?: 0,
                meta = countsObj?.badgeOptInt("meta") ?: 0,
                tracker = countsObj?.badgeOptInt("tracker") ?: 0,
                wifiAnomaly = countsObj?.badgeOptInt("wifi_anomaly") ?: 0,
                ble = countsObj?.badgeOptInt("ble") ?: 0,
                other = countsObj?.badgeOptInt("other") ?: 0
            ),
            entities = entities,
            scanners = scanners,
            displayPolicy = displayPolicy,
            displayPolicyHash = obj.badgeOptLong("display_policy_hash"),
            filteredCounts = filteredCounts,
            theme = theme,
            themeHash = obj.badgeOptLong("theme_hash"),
            displayState = displayState,
            bleControl = bleControl,
            safeMode = obj.badgeOptBoolean("safe_mode"),
            safeReason = obj.badgeOptString("safe_reason"),
            resetReason = obj.badgeOptString("reset_reason")
                .ifBlank { reportingObj?.badgeOptString("reset_reason").orEmpty() },
            resetReasonCode = obj.badgeOptLong("reset_reason_code").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("reset_reason_code")
                ?: 0L,
            resetExpected = if (obj.get("reset_expected") != null) {
                obj.badgeOptBoolean("reset_expected")
            } else {
                reportingObj?.badgeOptBoolean("reset_expected") ?: false
            },
            crashCount = obj.badgeOptInt("crash_count").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("crash_count")
                ?: 0,
            recoveryMode = obj.badgeOptString("recovery_mode")
                .ifBlank { reportingObj?.badgeOptString("recovery_mode").orEmpty() },
            usbControlAgeSeconds = obj.badgeOptLongOrNull("usb_control_age_s")
                ?: reportingObj?.badgeOptLongOrNull("usb_control_age_s"),
            stackMainFree = obj.badgeOptInt("stack_main_free").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("stack_main_free")
                ?: 0,
            stackDisplayFree = obj.badgeOptInt("stack_display_free").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("stack_display_free")
                ?: 0,
            stackUsbFree = obj.badgeOptInt("stack_usb_free").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("stack_usb_free")
                ?: 0,
            stackUartBleFree = obj.badgeOptInt("stack_uart_ble_free").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("stack_uart_ble_free")
                ?: 0,
            stackUartWifiFree = obj.badgeOptInt("stack_uart_wifi_free").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("stack_uart_wifi_free")
                ?: 0,
            heapInternalFree = obj.badgeOptLong("heap_internal_free").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("heap_internal_free")
                ?: 0L,
            heapInternalMinFree = obj.badgeOptLong("heap_internal_min_free").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("heap_internal_min_free")
                ?: 0L,
            heapInternalLargest = obj.badgeOptLong("heap_internal_largest").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("heap_internal_largest")
                ?: 0L,
            psramTotal = obj.badgeOptLong("psram_total").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("psram_total")
                ?: 0L,
            psramFree = obj.badgeOptLong("psram_free").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("psram_free")
                ?: 0L,
            psramLargest = obj.badgeOptLong("psram_largest").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("psram_largest")
                ?: 0L
        )
    }.getOrNull()
}

private fun parseBadgeUsbHealthStatus(obj: JsonObject?): BadgeUsbHealthStatus {
    if (obj == null) return BadgeUsbHealthStatus()
    return BadgeUsbHealthStatus(
        schema = obj.badgeStrictNonNegativeInt("schema") ?: 0,
        taskStarted = obj.badgeStrictBoolean("task_started") ?: false,
        hostConnected = obj.badgeStrictBoolean("host_connected") ?: false,
        parserState = obj.badgeStrictString("parser_state")
            ?.takeIf { it in BadgeUsbParserStates }
            .orEmpty(),
        rxBytes = obj.badgeStrictNonNegativeLong("rx_bytes") ?: 0L,
        validCommands = obj.badgeStrictNonNegativeLong("valid_commands") ?: 0L,
        responsesCompleted = obj.badgeStrictNonNegativeLong("responses_completed") ?: 0L,
        requiredResponseFailures =
            obj.badgeStrictNonNegativeLong("required_response_failures") ?: 0L,
        malformedLines = obj.badgeStrictNonNegativeLong("malformed_lines") ?: 0L,
        droppedProgressFrames =
            obj.badgeStrictNonNegativeLong("dropped_progress_frames") ?: 0L,
        droppedOptionalFrames =
            obj.badgeStrictNonNegativeLong("dropped_optional_frames") ?: 0L,
        uploadReceived = obj.badgeStrictNonNegativeLong("upload_received") ?: 0L,
        uploadSize = obj.badgeStrictNonNegativeLong("upload_size") ?: 0L,
        taskHeartbeatAgeSeconds =
            obj.badgeStrictNonNegativeLong("task_heartbeat_age_s"),
        lastRxAgeSeconds = obj.badgeStrictNonNegativeLong("last_rx_age_s"),
        lastCommandAgeSeconds = obj.badgeStrictNonNegativeLong("last_command_age_s"),
        lastResponseAgeSeconds = obj.badgeStrictNonNegativeLong("last_response_age_s"),
        lastUploadProgressAgeSeconds =
            obj.badgeStrictNonNegativeLong("last_upload_progress_age_s"),
    )
}

private val BadgeHardwareIdPattern =
    Regex("^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")

internal enum class BadgeUsbProductKind {
    NATIVE_BADGE,
    BADGE_LITE,
}

internal fun canonicalBadgeHardwareId(raw: String): String? {
    if (!BadgeHardwareIdPattern.matches(raw)) return null
    val canonical = raw.uppercase()
    if (canonical == "00:00:00:00:00:00" || canonical == "FF:FF:FF:FF:FF:FF") {
        return null
    }
    return canonical
}

private fun exactBadgeIdentityAlias(primary: String, alias: String): String? = when {
    primary.isBlank() -> alias.takeIf(String::isNotBlank)
    alias.isBlank() -> primary
    primary == alias -> primary
    else -> null
}

private fun exactBadgeHardwareIdAlias(primary: String, alias: String): String? {
    val primaryCanonical = primary.takeIf(String::isNotBlank)
        ?.let(::canonicalBadgeHardwareId)
    val aliasCanonical = alias.takeIf(String::isNotBlank)
        ?.let(::canonicalBadgeHardwareId)
    if ((primary.isNotBlank() && primaryCanonical == null) ||
        (alias.isNotBlank() && aliasCanonical == null)
    ) {
        return null
    }
    return when {
        primaryCanonical == null -> aliasCanonical
        aliasCanonical == null -> primaryCanonical
        primaryCanonical == aliasCanonical -> primaryCanonical
        else -> null
    }
}

internal fun badgeUsbProductKind(status: BadgeControlStatus?): BadgeUsbProductKind? {
    if (status == null) return null
    return when (status.productFamily) {
        "" -> BadgeUsbProductKind.NATIVE_BADGE
        "badge_lite" -> BadgeUsbProductKind.BADGE_LITE
        else -> null
    }
}

internal fun badgeDisplayControlsAvailable(status: BadgeControlStatus?): Boolean =
    status == null ||
        (status.mode != "headless" && "display_none" !in status.capabilities)

internal fun badgeUsbIdentityError(
    status: BadgeControlStatus?,
    expectedHardwareId: String? = null,
    expectedProductKind: BadgeUsbProductKind? = null,
): String? {
    // These fields are self-asserted by firmware. This is a product-routing safety check,
    // not cryptographic device authentication.
    if (status == null) return "Malformed badge status"
    val productKind = badgeUsbProductKind(status)
        ?: return "Unexpected USB product family: ${status.productFamily.ifBlank { "missing" }}"
    val hardwareId = when (productKind) {
        BadgeUsbProductKind.NATIVE_BADGE -> {
            if (status.firmwareName != "uplink-s3-fof_badge") {
                return "Unexpected USB firmware identity: ${status.firmwareName.ifBlank { "missing" }}"
            }
            if (status.appProject != "fof_badge_uplink") {
                return "Unexpected USB app project: ${status.appProject.ifBlank { "missing" }}"
            }
            if (status.hardwareType != "seeed_xiao_esp32s3") {
                return "Unexpected USB hardware identity: ${status.hardwareType.ifBlank { "missing" }}"
            }
            if (status.firmwareTarget.isNotBlank() &&
                status.firmwareTarget != "uplink-s3-fof_badge"
            ) {
                return "Unexpected USB target identity: ${status.firmwareTarget}"
            }
            canonicalBadgeHardwareId(status.hardwareId)
        }
        BadgeUsbProductKind.BADGE_LITE -> {
            if (status.firmwareTarget != "uplink-s3-backend") {
                return "Unexpected USB target identity: ${status.firmwareTarget.ifBlank { "missing" }}"
            }
            if (status.firmwareName.isNotBlank() &&
                status.firmwareName != "uplink-s3-backend"
            ) {
                return "Unexpected USB firmware identity: ${status.firmwareName}"
            }
            val project = exactBadgeIdentityAlias(status.project, status.appProject)
            if (project != "fof_backend_uplink") {
                return "Unexpected USB app project: ${project ?: "conflicting aliases"}"
            }
            val hardware = exactBadgeIdentityAlias(status.hardware, status.hardwareType)
            if (hardware != "seeed_xiao_esp32s3") {
                return "Unexpected USB hardware identity: ${hardware ?: "conflicting aliases"}"
            }
            if (status.mode != "headless") {
                return "Unexpected USB badge mode: ${status.mode.ifBlank { "missing" }}"
            }
            if ("display_none" !in status.capabilities) {
                return "Unexpected USB badge capabilities: display_none missing"
            }
            exactBadgeHardwareIdAlias(status.mac, status.hardwareId)
        }
    }
        ?: return "Unexpected USB hardware ID: missing or invalid"
    if (expectedProductKind != null && productKind != expectedProductKind) {
        return "USB product identity mismatch"
    }
    if (expectedHardwareId != null) {
        val expected = canonicalBadgeHardwareId(expectedHardwareId)
            ?: return "Expected USB hardware ID is invalid"
        if (hardwareId != expected) {
            return "USB hardware ID mismatch"
        }
    }
    return null
}

internal fun badgeUsbStatusFrameIdentityError(
    isStatusFrame: Boolean,
    status: BadgeControlStatus?,
    expectedHardwareId: String? = null,
    expectedProductKind: BadgeUsbProductKind? = null,
): String? = if (isStatusFrame) {
    badgeUsbIdentityError(status, expectedHardwareId, expectedProductKind)
} else {
    null
}

internal fun badgeUsbHandshakeStatus(status: BadgeControlStatus?): BadgeUsbStatus =
    if (badgeUsbIdentityError(status) == null) BadgeUsbStatus.CONNECTED else BadgeUsbStatus.ERROR

internal fun badgeUsbHandshakeTimeoutStatus(current: BadgeUsbStatus): BadgeUsbStatus =
    if (current == BadgeUsbStatus.CONNECTING) BadgeUsbStatus.ERROR else current

internal fun badgeUsbSessionOwnsTransport(
    status: BadgeUsbStatus,
    transportLabel: String,
    connectionOpen: Boolean,
): Boolean = connectionOpen &&
    transportLabel == "USB-C" &&
    (status == BadgeUsbStatus.CONNECTING || status == BadgeUsbStatus.CONNECTED)

internal fun badgeUsbVerifiedWriteAllowed(
    lifecycleActive: Boolean,
    status: BadgeUsbStatus,
    transportLabel: String,
    activeLifecycleSession: Long?,
    verifiedLifecycleSession: Long?,
    activeConnection: Any?,
    activeEndpoint: Any?,
    verifiedConnection: Any?,
    verifiedEndpoint: Any?,
): Boolean = lifecycleActive &&
    status == BadgeUsbStatus.CONNECTED &&
    transportLabel == "USB-C" &&
    activeLifecycleSession != null &&
    activeLifecycleSession == verifiedLifecycleSession &&
    activeConnection != null &&
    activeConnection === verifiedConnection &&
    activeEndpoint != null &&
    activeEndpoint === verifiedEndpoint

internal fun badgeUsbHandshakeOwnsSession(
    lifecycleActive: Boolean,
    status: BadgeUsbStatus,
    transportLabel: String,
    expectedLifecycleSession: Long,
    activeLifecycleSession: Long?,
    expectedConnection: Any,
    activeConnection: Any?,
): Boolean = lifecycleActive &&
    status == BadgeUsbStatus.CONNECTING &&
    transportLabel == "USB-C" &&
    expectedLifecycleSession == activeLifecycleSession &&
    expectedConnection === activeConnection

internal fun badgeUsbReaderOwnsExactSession(
    lifecycleActive: Boolean,
    expectedLifecycleSession: Long,
    activeLifecycleSession: Long?,
    expectedConnection: Any,
    activeConnection: Any?,
): Boolean = badgeUsbReaderOwnsSession(
    lifecycleActive = lifecycleActive,
    activeConnectionMatches = expectedConnection === activeConnection,
) && expectedLifecycleSession == activeLifecycleSession

internal fun badgeUsbReaderTerminalOwnsExactSession(
    expectedLifecycleSession: Long,
    expectedAttachmentToken: BadgeUsbAttachmentToken,
    expectedConnection: Any,
    lifecycleActive: Boolean,
    status: BadgeUsbStatus,
    transportLabel: String,
    activeLifecycleSession: Long?,
    activeAttachmentToken: BadgeUsbAttachmentToken?,
    attachmentCurrentAndActive: Boolean,
    activeConnection: Any?,
    activeEndpoint: Any?,
    expectedVerifiedOwner: BadgeUsbOwnerKey?,
    activeVerifiedOwner: BadgeUsbOwnerKey?,
): Boolean {
    if (transportLabel != "USB-C" ||
        !badgeUsbReaderOwnsExactSession(
            lifecycleActive = lifecycleActive,
            expectedLifecycleSession = expectedLifecycleSession,
            activeLifecycleSession = activeLifecycleSession,
            expectedConnection = expectedConnection,
            activeConnection = activeConnection,
        ) ||
        expectedAttachmentToken != activeAttachmentToken ||
        !attachmentCurrentAndActive
    ) {
        return false
    }
    return when (status) {
        BadgeUsbStatus.CONNECTING ->
            expectedVerifiedOwner == null && activeVerifiedOwner == null
        BadgeUsbStatus.CONNECTED ->
            expectedVerifiedOwner?.let { expectedOwner ->
                badgeUsbOwnerKeysMatch(expectedOwner, activeVerifiedOwner) &&
                    expectedOwner.attachmentToken == expectedAttachmentToken &&
                    expectedOwner.lifecycleSession == expectedLifecycleSession &&
                    expectedOwner.connectionIdentity === expectedConnection &&
                    expectedOwner.endpointIdentity === activeEndpoint
            } == true
        else -> false
    }
}

internal fun badgeUsbFrameMayMutateState(
    hasVerifiedOwner: Boolean,
    acceptedIdentityHandshake: Boolean,
): Boolean = hasVerifiedOwner || acceptedIdentityHandshake

internal data class BadgeUsbDeviceIdentity(
    val deviceId: Int,
    // Android's UsbDevice.deviceName is an OS path identity, not the human display label.
    val devicePath: String,
)

internal data class BadgeUsbAttachmentToken(
    val generation: Long,
    val identity: BadgeUsbDeviceIdentity,
)

internal data class BadgeUsbAttachmentInvalidation(
    val token: BadgeUsbAttachmentToken,
    val wasActive: Boolean,
)

internal class BadgeUsbAttachmentGate {
    private var nextGeneration = 1L
    private var selectedToken: BadgeUsbAttachmentToken? = null
    private var activeToken: BadgeUsbAttachmentToken? = null

    @Synchronized
    fun select(
        identity: BadgeUsbDeviceIdentity,
        forceNewGeneration: Boolean = false,
    ): BadgeUsbAttachmentToken {
        val current = selectedToken
        if (!forceNewGeneration && current?.identity == identity) return current
        val selected = BadgeUsbAttachmentToken(nextGeneration++, identity)
        selectedToken = selected
        return selected
    }

    @Synchronized
    fun isCurrent(token: BadgeUsbAttachmentToken): Boolean = selectedToken == token

    @Synchronized
    fun activate(token: BadgeUsbAttachmentToken): Boolean {
        if (selectedToken != token) return false
        activeToken = token
        return true
    }

    @Synchronized
    fun activateAndPublishIfCurrent(
        token: BadgeUsbAttachmentToken,
        publication: () -> Unit,
    ): Boolean {
        if (selectedToken != token) return false
        activeToken = token
        publication()
        return true
    }

    @Synchronized
    fun publishIfCurrentAndActive(
        token: BadgeUsbAttachmentToken,
        publication: () -> Unit,
    ): Boolean {
        if (selectedToken != token || activeToken != token) return false
        publication()
        return true
    }

    @Synchronized
    fun isCurrentAndActive(token: BadgeUsbAttachmentToken): Boolean =
        selectedToken == token && activeToken == token

    @Synchronized
    fun acceptsPermission(
        token: BadgeUsbAttachmentToken,
        identity: BadgeUsbDeviceIdentity,
    ): Boolean = selectedToken == token && token.identity == identity

    @Synchronized
    fun invalidateMatching(
        identity: BadgeUsbDeviceIdentity,
    ): BadgeUsbAttachmentInvalidation? {
        val selected = selectedToken?.takeIf { it.identity == identity }
        if (selected != null) {
            selectedToken = null
            return BadgeUsbAttachmentInvalidation(selected, activeToken == selected)
        }
        val active = activeToken?.takeIf { it.identity == identity } ?: return null
        return BadgeUsbAttachmentInvalidation(active, wasActive = true)
    }

    @Synchronized
    fun invalidateCurrent(): BadgeUsbAttachmentInvalidation? {
        val current = selectedToken ?: return null
        selectedToken = null
        return BadgeUsbAttachmentInvalidation(current, activeToken == current)
    }

    @Synchronized
    fun invalidateExact(token: BadgeUsbAttachmentToken): BadgeUsbAttachmentInvalidation? {
        if (selectedToken != token) return null
        selectedToken = null
        return BadgeUsbAttachmentInvalidation(token, activeToken == token)
    }

    @Synchronized
    fun clearActive(token: BadgeUsbAttachmentToken) {
        if (activeToken == token) activeToken = null
        if (selectedToken == token) selectedToken = null
    }

    @Synchronized
    fun deactivateExact(token: BadgeUsbAttachmentToken): Boolean {
        if (activeToken != token) return false
        activeToken = null
        return true
    }

    @Synchronized
    fun currentToken(): BadgeUsbAttachmentToken? = selectedToken
}

internal fun badgeUsbCleanupOwnsActive(
    expectedAttachmentToken: BadgeUsbAttachmentToken,
    expectedConnection: Any,
    activeAttachmentToken: BadgeUsbAttachmentToken?,
    activeConnection: Any?,
): Boolean = expectedAttachmentToken == activeAttachmentToken &&
    expectedConnection === activeConnection

internal fun badgeUsbConnectionCanBeReused(
    status: BadgeUsbStatus,
    activeAttachmentToken: BadgeUsbAttachmentToken?,
    requestedAttachmentToken: BadgeUsbAttachmentToken,
    activeLifecycleSession: Long?,
    requestedLifecycleSession: Long,
    connectionOpen: Boolean,
): Boolean = connectionOpen &&
    (status == BadgeUsbStatus.CONNECTING || status == BadgeUsbStatus.CONNECTED) &&
    activeAttachmentToken == requestedAttachmentToken &&
    activeLifecycleSession == requestedLifecycleSession

internal data class BadgeUsbOwnerKey(
    val attachmentToken: BadgeUsbAttachmentToken,
    val lifecycleSession: Long,
    val connectionIdentity: Any,
    val endpointIdentity: Any,
    val hardwareId: String,
    val productKind: BadgeUsbProductKind = BadgeUsbProductKind.NATIVE_BADGE,
)

internal fun badgeUsbOwnerKeyFromHandshake(
    status: BadgeControlStatus?,
    attachmentToken: BadgeUsbAttachmentToken,
    lifecycleSession: Long,
    connectionIdentity: Any,
    endpointIdentity: Any,
): BadgeUsbOwnerKey? {
    if (badgeUsbIdentityError(status) != null) return null
    val productKind = badgeUsbProductKind(status) ?: return null
    val hardwareId = when (productKind) {
        BadgeUsbProductKind.NATIVE_BADGE -> canonicalBadgeHardwareId(status?.hardwareId.orEmpty())
        BadgeUsbProductKind.BADGE_LITE -> exactBadgeHardwareIdAlias(
            status?.mac.orEmpty(),
            status?.hardwareId.orEmpty(),
        )
    } ?: return null
    return BadgeUsbOwnerKey(
        attachmentToken = attachmentToken,
        lifecycleSession = lifecycleSession,
        connectionIdentity = connectionIdentity,
        endpointIdentity = endpointIdentity,
        hardwareId = hardwareId,
        productKind = productKind,
    )
}

internal fun badgeUsbOwnerKeysMatch(
    expected: BadgeUsbOwnerKey?,
    actual: BadgeUsbOwnerKey?,
): Boolean = expected != null && actual != null &&
    expected.attachmentToken == actual.attachmentToken &&
    expected.lifecycleSession == actual.lifecycleSession &&
    expected.connectionIdentity === actual.connectionIdentity &&
    expected.endpointIdentity === actual.endpointIdentity &&
    expected.hardwareId == actual.hardwareId &&
    expected.productKind == actual.productKind

internal fun badgeUsbStatusResponseCounter(status: BadgeControlStatus?): Long? =
    status?.usbHealth?.takeIf { it.schema == 1 }?.responsesCompleted

internal fun badgeUsbTerminalFailureOwnsExactSession(
    expectedOwner: BadgeUsbOwnerKey,
    activeOwner: BadgeUsbOwnerKey?,
    lifecycleActive: Boolean,
    status: BadgeUsbStatus,
    transportLabel: String,
    activeLifecycleSession: Long?,
    activeAttachmentToken: BadgeUsbAttachmentToken?,
    attachmentCurrentAndActive: Boolean,
    activeConnection: Any?,
    activeEndpoint: Any?,
): Boolean = badgeUsbOwnerKeysMatch(expectedOwner, activeOwner) &&
    activeAttachmentToken == expectedOwner.attachmentToken &&
    attachmentCurrentAndActive &&
    badgeUsbVerifiedWriteAllowed(
        lifecycleActive = lifecycleActive,
        status = status,
        transportLabel = transportLabel,
        activeLifecycleSession = activeLifecycleSession,
        verifiedLifecycleSession = expectedOwner.lifecycleSession,
        activeConnection = activeConnection,
        activeEndpoint = activeEndpoint,
        verifiedConnection = expectedOwner.connectionIdentity,
        verifiedEndpoint = expectedOwner.endpointIdentity,
    )

internal class BadgeUsbInvestigationOwnershipGate(
    private val owner: BadgeUsbOwnerKey,
) {
    private var disconnected = false
    private var acceptedFrames = 0

    @Synchronized
    fun acceptsFrame(frameOwner: BadgeUsbOwnerKey?, status: BadgeUsbStatus): Boolean {
        if (disconnected || status != BadgeUsbStatus.CONNECTED ||
            !badgeUsbOwnerKeysMatch(owner, frameOwner)
        ) {
            return false
        }
        acceptedFrames++
        return true
    }

    @Synchronized
    fun disconnect(disconnectedOwner: BadgeUsbOwnerKey?): Boolean {
        if (disconnected || !badgeUsbOwnerKeysMatch(owner, disconnectedOwner)) return false
        disconnected = true
        return true
    }

    @Synchronized
    fun isDisconnected(): Boolean = disconnected

    @Synchronized
    fun terminalError(): String? = if (disconnected) "transport_disconnected" else null

    @Synchronized
    fun acceptedFrameCount(): Int = acceptedFrames
}

internal fun reduceBadgeHttpStatus(
    current: BadgeUsbState,
    response: BadgeControlStatus,
    connectedStatus: BadgeUsbStatus,
    deviceName: String,
    transportLabel: String,
    connectedMessage: String,
    usbConnectionOpen: Boolean,
): BadgeUsbState {
    if (badgeUsbStateReservesUsb(current, usbConnectionOpen)) {
        return current
    }
    return current.copy(
        status = connectedStatus,
        deviceName = deviceName,
        message = connectedMessage,
        transportLabel = transportLabel,
        controlStatus = response,
    )
}

internal fun badgeUsbStateReservesUsb(
    current: BadgeUsbState,
    usbConnectionOpen: Boolean,
): Boolean = badgeUsbSessionOwnsTransport(
    status = current.status,
    transportLabel = current.transportLabel,
    connectionOpen = usbConnectionOpen,
) || (
    current.transportLabel == "USB-C" && current.status in setOf(
        BadgeUsbStatus.PERMISSION_NEEDED,
        BadgeUsbStatus.CONNECTING,
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.ERROR,
    )
)

internal fun reduceBadgeUsbDisconnected(
    current: BadgeUsbState,
    reason: String,
): BadgeUsbState = current.copy(
    status = BadgeUsbStatus.DISCONNECTED,
    deviceName = null,
    message = reason,
    transportLabel = "",
    controlStatus = null,
)

internal fun reduceBadgeUsbReaderFailure(
    current: BadgeUsbState,
    deviceName: String,
    detail: String,
): BadgeUsbState = reduceBadgeUsbTerminalError(
    current = current,
    deviceName = deviceName,
    message = "Badge USB read failed: $detail",
)

internal fun reduceBadgeUsbTerminalError(
    current: BadgeUsbState,
    deviceName: String,
    message: String,
): BadgeUsbState = current.copy(
    status = BadgeUsbStatus.ERROR,
    deviceName = deviceName,
    message = message,
    transportLabel = "USB-C",
    controlStatus = null,
)

internal enum class BadgeUsbHandshakeTimerAction {
    RETRY,
    FAIL,
    STOP,
}

internal fun badgeUsbHandshakeDelayMs(
    nowElapsedMs: Long,
    deadlineElapsedMs: Long,
    retryIntervalMs: Long,
    retryWriteBudgetMs: Long,
): Long {
    if (nowElapsedMs >= deadlineElapsedMs) return 0L
    val remainingBeforeWriteBudget =
        (deadlineElapsedMs - nowElapsedMs - retryWriteBudgetMs.coerceAtLeast(0L))
            .coerceAtLeast(0L)
    return remainingBeforeWriteBudget.coerceAtMost(retryIntervalMs.coerceAtLeast(0L))
}

internal fun badgeUsbHandshakeTimerAction(
    ownsSession: Boolean,
    nowElapsedMs: Long,
    deadlineElapsedMs: Long,
    retryWriteBudgetMs: Long,
): BadgeUsbHandshakeTimerAction = when {
    !ownsSession -> BadgeUsbHandshakeTimerAction.STOP
    nowElapsedMs >= deadlineElapsedMs ||
        deadlineElapsedMs - nowElapsedMs <= retryWriteBudgetMs.coerceAtLeast(0L) ->
        BadgeUsbHandshakeTimerAction.FAIL
    else -> BadgeUsbHandshakeTimerAction.RETRY
}

internal fun badgeUsbReaderOwnsSession(
    lifecycleActive: Boolean,
    activeConnectionMatches: Boolean,
): Boolean = lifecycleActive && activeConnectionMatches

internal suspend fun Mutex.withBadgeUsbReaderOwner(
    owns: () -> Boolean,
    action: () -> Unit,
): Boolean = withLock {
    if (!owns()) {
        false
    } else {
        action()
        true
    }
}

private fun parseBadgeDisplayPolicy(obj: JsonObject?): BadgeDisplayPolicy {
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
                enabled = classObj.badgeOptBoolean("enabled", fallback.enabled),
                lane = classObj.badgeOptString("lane").ifBlank { fallback.lane },
                minProximity = classObj.badgeOptString("min_proximity")
                    .ifBlank { fallback.minProximity },
                priority = classObj.badgeOptInt("priority", fallback.priority).coerceIn(0, 100)
            )
        }
    }
    return BadgeDisplayPolicy(
        version = obj.badgeOptInt("version", 1),
        classes = parsed
    )
}

private fun parseBadgeIntMap(obj: JsonObject?): Map<String, Int> {
    if (obj == null) return emptyMap()
    return obj.entrySet().associate { (key, value) ->
        key to runCatching { value.asInt }.getOrDefault(0)
    }
}

private fun parseBadgeTheme(obj: JsonObject?): BadgeTheme {
    if (obj == null) return defaultBadgeTheme()
    val defaults = defaultBadgeTheme()
    val accents = defaults.accents.toMutableMap()
    val accentsObj = runCatching { obj.getAsJsonObject("accents") }.getOrNull()
    BadgeThemeAccentClasses.forEach { info ->
        val value = runCatching {
            accentsObj?.get(info.key)?.takeIf { !it.isJsonNull }?.asInt
        }.getOrNull()
        if (value != null) {
            accents[info.key] = value.coerceIn(0, 0xffff)
        }
    }
    return BadgeTheme(
        version = obj.badgeOptInt("version", 1),
        palette = obj.badgeOptString("palette")
            .takeIf { it in BadgeThemePalettes } ?: defaults.palette,
        background = obj.badgeOptString("background")
            .takeIf { it in BadgeThemeBackgrounds } ?: defaults.background,
        brightness = obj.badgeOptInt("brightness", defaults.brightness).coerceIn(25, 100),
        accents = accents
    )
}

private fun parseBadgeBleControlStatus(obj: JsonObject?): BadgeBleControlStatus {
    if (obj == null) return BadgeBleControlStatus()
    return BadgeBleControlStatus(
        enabled = obj.badgeOptBoolean("enabled"),
        bonded = obj.badgeOptBoolean("bonded"),
        pairingAgeSeconds = obj.badgeOptLongOrNull("pairing_age_s"),
        pairingWindowSeconds = obj.badgeOptInt("pairing_window_s", 10),
        connected = obj.badgeOptBoolean("connected"),
        encrypted = obj.badgeOptBoolean("encrypted"),
        lastError = obj.badgeOptString("last_error"),
        rx = obj.badgeOptLong("rx"),
        tx = obj.badgeOptLong("tx")
    )
}

private fun parseBadgeDisplayState(obj: JsonObject?): BadgeDisplayState? {
    if (obj == null) return null
    return BadgeDisplayState(
        active = obj.badgeOptBoolean("active"),
        detailMode = obj.badgeOptBoolean("detail_mode"),
        detailPage = obj.badgeOptInt("detail_page"),
        focusIndex = obj.badgeOptInt("focus_index"),
        focusTotal = obj.badgeOptInt("focus_total"),
        itemIndex = obj.badgeOptInt("item_index"),
        itemTotal = obj.badgeOptInt("item_total"),
        lane = obj.badgeOptString("lane"),
        title = obj.badgeOptString("title"),
        detail = obj.badgeOptString("detail"),
        evidence = obj.badgeOptString("evidence"),
        entityKey = obj.badgeOptString("entity_key"),
        displayId = obj.badgeOptString("display_id"),
        threatClass = obj.badgeOptString("class"),
        category = obj.badgeOptString("category"),
        code = obj.badgeOptString("code"),
        source = obj.badgeOptString("source"),
        ssid = obj.badgeOptString("ssid"),
        bssid = obj.badgeOptString("bssid"),
        authMode = obj.badgeOptInt("auth_m", -1),
        freqMhz = obj.badgeOptInt("freq_mhz"),
        score = obj.badgeOptInt("score"),
        confidencePct = obj.badgeOptInt("confidence_pct"),
        evidenceQuality = obj.badgeOptInt("evidence_quality"),
        displayRank = obj.badgeOptInt("display_rank"),
        ageSeconds = obj.badgeOptInt("age_s"),
        lastSeenSeconds = obj.badgeOptInt("last_seen_s"),
        rssi = obj.badgeOptInt("rssi"),
        bestRssi = obj.badgeOptInt("best_rssi"),
        events = obj.badgeOptInt("events"),
        seenCount = obj.badgeOptInt("seen_count"),
        groupCount = obj.badgeOptInt("group_count"),
        proximityLevel = obj.badgeOptInt("proximity_level"),
        stale = obj.badgeOptBoolean("stale"),
        lat = obj.badgeOptDoubleOrNull("lat"),
        lon = obj.badgeOptDoubleOrNull("lon"),
        altitudeM = obj.badgeOptFloatOrNull("altitude_m"),
        operatorLat = obj.badgeOptDoubleOrNull("operator_lat"),
        operatorLon = obj.badgeOptDoubleOrNull("operator_lon"),
        operatorId = obj.badgeOptString("operator_id").ifBlank { null }
    )
}

private fun JsonObject.badgeOptString(key: String): String {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asString.orEmpty() }
        .getOrDefault("")
}

private fun JsonObject.badgeStrictString(key: String): String? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    return primitive.takeIf { it.isString }?.asString
}

private fun JsonObject.badgeStrictStringSet(key: String): Set<String> {
    val array = runCatching { getAsJsonArray(key) }.getOrNull() ?: return emptySet()
    val values = LinkedHashSet<String>()
    array.forEach { element ->
        val value = element
            .takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString
            ?.takeIf(String::isNotBlank)
            ?: return emptySet()
        values += value
    }
    return values
}

private fun JsonObject.badgeStrictNonNegativeInt(key: String): Int? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    return primitive.takeIf { it.isNumber }?.asString?.toIntOrNull()?.takeIf { it >= 0 }
}

private fun JsonObject.badgeStrictNonNegativeLong(key: String): Long? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    return primitive.takeIf { it.isNumber }?.asString?.toLongOrNull()?.takeIf { it >= 0L }
}

private fun JsonObject.badgeStrictBoolean(key: String): Boolean? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    return primitive.takeIf { it.isBoolean }?.asBoolean
}

private fun JsonObject.badgeOptInt(key: String, fallback: Int = 0): Int {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asInt ?: fallback }
        .getOrDefault(fallback)
}

private fun JsonObject.badgeOptLong(key: String, fallback: Long = 0L): Long {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong ?: fallback }
        .getOrDefault(fallback)
}

private fun JsonObject.badgeOptLongOrNull(key: String): Long? {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong }.getOrNull()
}

private fun JsonObject.badgeOptFloat(key: String, fallback: Float = 0f): Float {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat ?: fallback }
        .getOrDefault(fallback)
}

private fun JsonObject.badgeOptFloatOrNull(key: String): Float? {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat }.getOrNull()
}

private fun JsonObject.badgeOptDoubleOrNull(key: String): Double? {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asDouble }.getOrNull()
}

private fun JsonObject.badgeOptBoolean(key: String, fallback: Boolean = false): Boolean {
    return runCatching { get(key)?.takeIf { !it.isJsonNull }?.asBoolean ?: fallback }
        .getOrDefault(fallback)
}

internal const val BADGE_INVESTIGATION_JSON_MAX_BYTES = 1023
internal const val BADGE_INVESTIGATION_LINE_MAX_BYTES = 8 + BADGE_INVESTIGATION_JSON_MAX_BYTES
internal const val BADGE_INVESTIGATION_TOTAL_TIMEOUT_MS = 12_000L

internal fun badgeInvestigationTotalTimeoutMs(requestedTimeoutMs: Long): Long =
    requestedTimeoutMs.coerceIn(1L, BADGE_INVESTIGATION_TOTAL_TIMEOUT_MS)

private val BadgeInvestigationMacPattern =
    Regex("^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")

private fun badgeInvestigationRequestIdValid(requestId: String): Boolean =
    requestId.length in 1..32 &&
        requestId.all { it.code in 0x21..0x7e }

internal fun badgeInvestigationCommandJson(
    request: com.friendorfoe.detection.BleInvestigationRequest,
): JsonObject? {
    if (request.route != com.friendorfoe.detection.BleInvestigationRoute.BADGE ||
        !badgeInvestigationRequestIdValid(request.requestId)
    ) return null

    val mode = when (request.target.mode) {
        BleInvestigationMode.GATT -> {
            if (request.target.mac?.matches(BadgeInvestigationMacPattern) != true) {
                return null
            }
            "gatt"
        }
        BleInvestigationMode.PASSIVE_CAPTURE -> {
            if (request.target.mac != null) return null
            "passive_capture"
        }
    }
    return JsonObject().apply {
        addProperty("cmd", "ble_investigate")
        addProperty("request_id", request.requestId)
        addProperty("mode", mode)
        request.target.mac?.let {
            addProperty("target", it.uppercase(java.util.Locale.ROOT))
        }
            ?: add("target", com.google.gson.JsonNull.INSTANCE)
        addProperty("timeout_ms", badgeInvestigationTotalTimeoutMs(request.timeoutMs))
    }
}

internal fun badgeBleInvestigationChunkCommandJson(
    requestId: String,
    seq: Int,
): JsonObject? {
    if (!badgeInvestigationRequestIdValid(requestId) || seq !in 0..63) {
        return null
    }
    return JsonObject().apply {
        addProperty("cmd", "ble_investigation_chunk")
        addProperty("request_id", requestId)
        addProperty("seq", seq)
    }
}

internal enum class BadgeHttpInvestigationAction { WAIT, RETRIEVE, FAIL }

internal data class BadgeHttpInvestigationDecision(
    val action: BadgeHttpInvestigationAction,
    val state: BleInvestigationState? = null,
    val summary: String = "",
    val error: String? = null,
    val authenticationRequired: Boolean = false,
    val truncated: Boolean = false,
)

internal fun evaluateBadgeHttpInvestigationStatus(
    json: String,
    expectedRequestId: String,
): BadgeHttpInvestigationDecision {
    fun malformed() = BadgeHttpInvestigationDecision(
        action = BadgeHttpInvestigationAction.FAIL,
        error = "malformed_status",
    )

    val root = runCatching { JsonParser.parseString(json) }.getOrNull()
        ?.takeIf { it.isJsonObject }
        ?.asJsonObject
        ?: return malformed()
    val status = root.get("ble_investigation")
        ?.takeIf { it.isJsonObject }
        ?.asJsonObject
        ?: return malformed()
    fun stringField(key: String, maxBytes: Int, required: Boolean): String? {
        val element = status.get(key) ?: return if (required) null else ""
        if (!element.isJsonPrimitive || !element.asJsonPrimitive.isString) return null
        return element.asString.takeIf { it.toByteArray(Charsets.UTF_8).size <= maxBytes }
    }

    val requestElement = status.get("request_id") ?: return malformed()
    if (!requestElement.isJsonPrimitive || !requestElement.asJsonPrimitive.isString) return malformed()
    val requestId = requestElement.asString
    if (requestId != expectedRequestId) {
        return BadgeHttpInvestigationDecision(BadgeHttpInvestigationAction.WAIT)
    }
    if (requestId.length !in 1..32 || requestId.any { it.code !in 0x21..0x7E }) return malformed()
    val stateName = stringField("state", 16, required = true) ?: return malformed()
    val state = when (stateName) {
        "idle" -> BleInvestigationState.IDLE
        "queued" -> BleInvestigationState.QUEUED
        "scanning" -> BleInvestigationState.SCANNING
        "connecting" -> BleInvestigationState.CONNECTING
        "discovering" -> BleInvestigationState.DISCOVERING
        "reading" -> BleInvestigationState.READING
        "complete" -> BleInvestigationState.COMPLETE
        "failed" -> BleInvestigationState.FAILED
        "cancelled" -> BleInvestigationState.CANCELLED
        else -> return malformed()
    }
    val summary = stringField("summary", 127, required = false) ?: return malformed()
    val errorElement = status.get("error")
    val error = when {
        errorElement == null -> null
        !errorElement.isJsonPrimitive || !errorElement.asJsonPrimitive.isString -> return malformed()
        errorElement.asString.toByteArray(Charsets.UTF_8).size > 63 -> return malformed()
        else -> errorElement.asString.ifBlank { null }
    }
    fun booleanField(key: String): Boolean? {
        val element = status.get(key) ?: return false
        if (!element.isJsonPrimitive || !element.asJsonPrimitive.isBoolean) return null
        return element.asBoolean
    }
    val authenticationRequired = booleanField("authentication_required") ?: return malformed()
    val truncated = booleanField("truncated") ?: return malformed()

    val action = if (state in setOf(
            BleInvestigationState.COMPLETE,
            BleInvestigationState.FAILED,
            BleInvestigationState.CANCELLED,
        )
    ) {
        BadgeHttpInvestigationAction.RETRIEVE
    } else {
        BadgeHttpInvestigationAction.WAIT
    }
    return BadgeHttpInvestigationDecision(
        action = action,
        state = state,
        summary = summary,
        error = error,
        authenticationRequired = authenticationRequired,
        truncated = truncated,
    )
}

internal fun finishBadgeHttpInvestigationFromStatus(
    parser: BadgeInvestigationStreamParser,
    requestId: String,
    status: BadgeHttpInvestigationDecision,
): BleInvestigationResult? {
    if (status.action != BadgeHttpInvestigationAction.RETRIEVE) return null
    val terminalState = when (status.state) {
        BleInvestigationState.FAILED -> BleInvestigationState.FAILED
        BleInvestigationState.CANCELLED -> BleInvestigationState.CANCELLED
        BleInvestigationState.COMPLETE -> BleInvestigationState.FAILED
        else -> return null
    }
    val summary = when (status.state) {
        BleInvestigationState.COMPLETE -> "Badge investigation ended without a terminal chunk"
        BleInvestigationState.CANCELLED -> status.summary.ifBlank { "Badge investigation was cancelled" }
        else -> status.summary.ifBlank { "Badge investigation failed" }
    }
    val error = when (status.state) {
        BleInvestigationState.COMPLETE -> "missing_terminal"
        BleInvestigationState.CANCELLED -> status.error ?: "cancelled"
        else -> status.error ?: "badge_failed"
    }
    val terminal = JsonObject().apply {
        addProperty("type", "ble_inv_end")
        addProperty("request_id", requestId)
        addProperty("state", terminalState.name.lowercase())
        addProperty("summary", summary)
        addProperty("error", error)
        addProperty("authentication_required", status.authenticationRequired)
        addProperty("truncated", status.truncated)
    }
    return parser.accept("FOF_INV:$terminal").result
}

internal fun decodeBadgeUtf8(bytes: ByteArray, length: Int): String? {
    if (length !in 0..bytes.size) return null
    return runCatching {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes, 0, length))
            .toString()
    }.getOrNull()
}

internal data class BadgeInvestigationParseResult(
    val accepted: Boolean,
    val chunk: BleInvestigationChunk? = null,
    val result: BleInvestigationResult? = null,
)

internal class BadgeInvestigationStreamParser(
    expectedRequestId: String? = null,
) {
    private var expectedRequestId = expectedRequestId?.takeIf(::validRequestId)
    private var activeRequestId: String? = null
    private var assembler: BleInvestigationChunkAssembler? = null
    private var terminal = false
    private var progressState = BleInvestigationState.IDLE
    private var nextServiceIndex = 0
    private var nextCharacteristicIndex = 0
    private var nextReadIndex = 0

    fun accept(line: String): BadgeInvestigationParseResult {
        if (!line.startsWith(PREFIX) || '\n' in line || '\r' in line ||
            line.toByteArray(Charsets.UTF_8).size > BADGE_INVESTIGATION_LINE_MAX_BYTES
        ) {
            return REJECTED
        }
        val payload = line.removePrefix(PREFIX)
        if (payload.isBlank() ||
            payload.toByteArray(Charsets.UTF_8).size > BADGE_INVESTIGATION_JSON_MAX_BYTES
        ) return REJECTED
        val root = runCatching { JsonParser.parseString(payload) }.getOrNull()
            ?.takeIf { it.isJsonObject }
            ?.asJsonObject
            ?: return REJECTED
        val chunk = parseChunk(root) ?: return REJECTED
        if (terminal) return REJECTED

        val configuredRequestId = expectedRequestId
        if (configuredRequestId != null && chunk.requestId != configuredRequestId) {
            return REJECTED
        }

        if (chunk is BleInvestigationChunk.Begin) {
            if (activeRequestId != null) return REJECTED
            expectedRequestId = configuredRequestId ?: chunk.requestId
            activeRequestId = chunk.requestId
            assembler = BleInvestigationChunkAssembler(chunk.requestId)
            progressState = BleInvestigationState.QUEUED
            nextServiceIndex = 0
            nextCharacteristicIndex = 0
            nextReadIndex = 0
        } else if (activeRequestId != chunk.requestId || assembler == null) {
            return REJECTED
        }

        if (!isNextChunkValid(chunk)) return REJECTED
        val result = assembler?.accept(chunk)
        if (chunk is BleInvestigationChunk.End && result == null) return REJECTED
        advance(chunk)
        if (result != null) terminal = true
        return BadgeInvestigationParseResult(accepted = true, chunk = chunk, result = result)
    }

    fun disconnect(requestId: String): BleInvestigationResult? {
        if (terminal || activeRequestId != requestId || assembler == null) return null
        val result = assembler?.accept(
            BleInvestigationChunk.End(
                requestId = requestId,
                state = "failed",
                summary = "Badge transport disconnected",
                error = "transport_disconnected",
            ),
        )
        if (result != null) terminal = true
        return result
    }

    fun reconnect(requestId: String) {
        if (expectedRequestId != null && expectedRequestId != requestId) return
        expectedRequestId = requestId.takeIf(::validRequestId) ?: return
        activeRequestId = null
        assembler = null
        terminal = false
        progressState = BleInvestigationState.IDLE
        nextServiceIndex = 0
        nextCharacteristicIndex = 0
        nextReadIndex = 0
    }

    private fun isNextChunkValid(chunk: BleInvestigationChunk): Boolean = when (chunk) {
        is BleInvestigationChunk.Begin -> true
        is BleInvestigationChunk.Progress ->
            chunk.state in PROGRESS_STATES && chunk.state.ordinal >= progressState.ordinal
        is BleInvestigationChunk.Service -> chunk.index == nextServiceIndex
        is BleInvestigationChunk.Characteristic -> chunk.index == nextCharacteristicIndex
        is BleInvestigationChunk.Read -> chunk.index == nextReadIndex
        is BleInvestigationChunk.End -> true
    }

    private fun advance(chunk: BleInvestigationChunk) {
        when (chunk) {
            is BleInvestigationChunk.Begin -> Unit
            is BleInvestigationChunk.Progress -> progressState = chunk.state
            is BleInvestigationChunk.Service -> nextServiceIndex++
            is BleInvestigationChunk.Characteristic -> nextCharacteristicIndex++
            is BleInvestigationChunk.Read -> nextReadIndex++
            is BleInvestigationChunk.End -> Unit
        }
    }

    private fun parseChunk(root: JsonObject): BleInvestigationChunk? {
        val type = root.strictString("type", MAX_TYPE_CHARS) ?: return null
        val requestId = root.strictString("request_id", MAX_REQUEST_ID_CHARS)
            ?.takeIf(::validRequestId)
            ?: return null
        return when (type) {
            "ble_inv_begin" -> {
                val mode = when (root.strictString("mode", MAX_MODE_CHARS)) {
                    "gatt" -> BleInvestigationMode.GATT
                    "passive_capture" -> BleInvestigationMode.PASSIVE_CAPTURE
                    else -> return null
                }
                val target = root.strictNullableString("target_mac", MAX_MAC_CHARS)
                    ?: if (root.has("target_mac") && !root.get("target_mac").isJsonNull) return null else null
                if (target != null && !MAC_REGEX.matches(target)) return null
                if (mode == BleInvestigationMode.GATT && target == null) return null
                if (mode == BleInvestigationMode.PASSIVE_CAPTURE && target != null) return null
                BleInvestigationChunk.Begin(requestId, mode, target)
            }
            "ble_inv_progress" -> {
                val state = root.strictString("state", MAX_STATE_CHARS)
                    ?.let { value ->
                        PROGRESS_STATES.firstOrNull { it.name.equals(value, ignoreCase = true) }
                    }
                    ?: return null
                BleInvestigationChunk.Progress(requestId, state)
            }
            "ble_inv_service" -> BleInvestigationChunk.Service(
                requestId = requestId,
                index = root.strictIndex() ?: return null,
                uuid = root.strictUuid("uuid") ?: return null,
            )
            "ble_inv_char" -> {
                val propertiesElement = root.get("properties")
                    ?.takeIf { it.isJsonArray }
                    ?.asJsonArray
                    ?: return null
                if (propertiesElement.size() > MAX_PROPERTIES) return null
                val properties = linkedSetOf<String>()
                propertiesElement.forEach { item ->
                    if (!item.isJsonPrimitive || !item.asJsonPrimitive.isString) return null
                    val property = item.asString
                    if (property !in KNOWN_PROPERTIES || !properties.add(property)) return null
                }
                BleInvestigationChunk.Characteristic(
                    requestId = requestId,
                    index = root.strictIndex() ?: return null,
                    serviceUuid = root.strictUuid("service_uuid") ?: return null,
                    uuid = root.strictUuid("uuid") ?: return null,
                    properties = properties,
                )
            }
            "ble_inv_read" -> {
                val valueHex = root.strictString(
                    "value_hex",
                    MAX_VALUE_HEX_CHARS,
                    allowEmpty = true,
                )
                    ?.takeIf { it.length % 2 == 0 && VALUE_HEX_REGEX.matches(it) }
                    ?: return null
                BleInvestigationChunk.Read(
                    requestId = requestId,
                    index = root.strictIndex() ?: return null,
                    uuid = root.strictUuid("uuid") ?: return null,
                    valueHex = valueHex,
                )
            }
            "ble_inv_end" -> {
                val state = root.strictString("state", MAX_STATE_CHARS)
                    ?.takeIf { it in TERMINAL_STATE_NAMES }
                    ?: return null
                val summary = root.strictString(
                    "summary",
                    MAX_SUMMARY_CHARS,
                    allowEmpty = true,
                ) ?: return null
                val error = root.strictNullableString("error", MAX_ERROR_CHARS)
                    ?: if (root.has("error") && !root.get("error").isJsonNull) return null else null
                val authenticationRequired = root.strictBoolean(
                    "authentication_required",
                    fallback = false,
                ) ?: return null
                val truncated = root.strictBoolean("truncated", fallback = false) ?: return null
                BleInvestigationChunk.End(
                    requestId = requestId,
                    state = state,
                    summary = summary,
                    error = error,
                    authenticationRequired = authenticationRequired,
                    truncated = truncated,
                )
            }
            else -> null
        }
    }

    private fun JsonObject.strictString(
        key: String,
        maxChars: Int,
        allowEmpty: Boolean = false,
    ): String? {
        val item = get(key) ?: return null
        if (!item.isJsonPrimitive || !item.asJsonPrimitive.isString) return null
        return item.asString.takeIf {
            (allowEmpty || it.isNotEmpty()) && it.toByteArray(Charsets.UTF_8).size <= maxChars
        }
    }

    private fun JsonObject.strictNullableString(key: String, maxChars: Int): String? {
        val item = get(key) ?: return null
        if (item.isJsonNull) return null
        if (!item.isJsonPrimitive || !item.asJsonPrimitive.isString) return null
        return item.asString.takeIf { it.toByteArray(Charsets.UTF_8).size <= maxChars }
    }

    private fun JsonObject.strictBoolean(key: String, fallback: Boolean): Boolean? {
        val item = get(key) ?: return fallback
        if (!item.isJsonPrimitive || !item.asJsonPrimitive.isBoolean) return null
        return item.asBoolean
    }

    private fun JsonObject.strictIndex(): Int? {
        val item = get("index") ?: return null
        if (!item.isJsonPrimitive || !item.asJsonPrimitive.isNumber) return null
        val number = runCatching { item.asDouble }.getOrNull() ?: return null
        if (!number.isFinite() || number % 1.0 != 0.0 || number !in 0.0..MAX_CHUNK_INDEX.toDouble()) {
            return null
        }
        return number.toInt()
    }

    private fun JsonObject.strictUuid(key: String): String? =
        strictString(key, MAX_UUID_CHARS)?.takeIf(UUID_REGEX::matches)

    private fun validRequestId(value: String): Boolean =
        value.length in 1..MAX_REQUEST_ID_CHARS && value.all { it.code in 0x21..0x7E }

    private companion object {
        const val PREFIX = "FOF_INV:"
        const val MAX_REQUEST_ID_CHARS = 32
        const val MAX_TYPE_CHARS = 24
        const val MAX_MODE_CHARS = 24
        const val MAX_STATE_CHARS = 16
        const val MAX_MAC_CHARS = 17
        const val MAX_UUID_CHARS = 36
        const val MAX_SUMMARY_CHARS = 127
        const val MAX_ERROR_CHARS = 63
        const val MAX_VALUE_HEX_CHARS = 128
        const val MAX_CHUNK_INDEX = 63
        const val MAX_PROPERTIES = 8

        val REJECTED = BadgeInvestigationParseResult(accepted = false)
        val MAC_REGEX = Regex("^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$")
        val UUID_REGEX = Regex(
            "^(?:[0-9A-F]{4}|[0-9A-F]{8}|[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})$",
        )
        val VALUE_HEX_REGEX = Regex("^[0-9A-F]*$")
        val KNOWN_PROPERTIES = setOf(
            "broadcast",
            "read",
            "write_without_response",
            "write",
            "notify",
            "indicate",
            "authenticated_signed_writes",
            "extended_properties",
        )
        val PROGRESS_STATES = setOf(
            BleInvestigationState.QUEUED,
            BleInvestigationState.SCANNING,
            BleInvestigationState.CONNECTING,
            BleInvestigationState.DISCOVERING,
            BleInvestigationState.READING,
        )
        val TERMINAL_STATE_NAMES = setOf("complete", "failed", "cancelled")
    }
}

private enum class BadgeInvestigationTransport(val resultName: String) {
    USB("badge-usb"),
    HTTP("badge-http"),
    BLE("badge-ble"),
}

internal data class BadgeControlAck(
    val accepted: Boolean,
    val error: String? = null,
)

internal class BadgeUsbInvestigationAckGate(
    private val requestId: String,
) {
    private var completed = false

    @Suppress("UNUSED_PARAMETER")
    fun accept(
        line: String,
        parsed: BadgeInvestigationParseResult?,
        ownsControlReply: Boolean,
    ): BadgeControlAck? {
        if (completed) return null
        val begin = parsed?.chunk as? BleInvestigationChunk.Begin
        if (parsed?.accepted == true && begin?.requestId == requestId) {
            completed = true
            return BadgeControlAck(accepted = true)
        }
        return null
    }
}

internal fun badgeGattCallbackMatches(
    expectedGatt: Any?,
    actualGatt: Any?,
    expectedKind: String,
    actualKind: String,
    expectedUuid: UUID,
    actualUuid: UUID,
    expectedGeneration: Long?,
    activeGeneration: Long?,
): Boolean = expectedGatt === actualGatt &&
    expectedKind == actualKind &&
    expectedUuid == actualUuid &&
    (expectedGeneration == null || expectedGeneration == activeGeneration)

internal fun badgeGattDisconnectMatches(activeGatt: Any?, callbackGatt: Any?): Boolean =
    activeGatt === callbackGatt

internal fun shouldStartBadgeInvestigationJob(
    activeGeneration: Long?,
    operationGeneration: Long,
): Boolean = activeGeneration == operationGeneration

private data class ActiveBadgeInvestigation(
    val generation: Long,
    val request: com.friendorfoe.detection.BleInvestigationRequest,
    val command: JsonObject,
    val transport: BadgeInvestigationTransport,
    val parser: BadgeInvestigationStreamParser,
    val usbOwnerKey: BadgeUsbOwnerKey? = null,
    val usbOwnershipGate: BadgeUsbInvestigationOwnershipGate? =
        usbOwnerKey?.let(::BadgeUsbInvestigationOwnershipGate),
    val ackGate: BadgeUsbInvestigationAckGate = BadgeUsbInvestigationAckGate(request.requestId),
    val controlAck: CompletableDeferred<BadgeControlAck> = CompletableDeferred(),
    val terminal: CompletableDeferred<BleInvestigationResult> = CompletableDeferred(),
    var job: Job? = null,
)

private enum class BadgeGattOperationKind { READ_CHARACTERISTIC, WRITE_CHARACTERISTIC, WRITE_DESCRIPTOR }

private data class BadgeGattOperationResult(
    val status: Int,
    val value: ByteArray = byteArrayOf(),
)

private data class PendingBadgeGattOperation(
    val gatt: BluetoothGatt,
    val kind: BadgeGattOperationKind,
    val uuid: UUID,
    val generation: Long?,
    val completion: CompletableDeferred<BadgeGattOperationResult> = CompletableDeferred(),
)

private data class ActiveBadgeUsbReconnectAttempt(
    val ticket: BadgeUsbReconnectTicket,
    val lifecycleSession: Long,
    val attachmentToken: BadgeUsbAttachmentToken,
    val connectionIdentity: Any? = null,
)

private data class ActiveBadgeUsbPermissionOperation(
    val generation: Long,
    val lifecycleSession: Long,
    val attachmentToken: BadgeUsbAttachmentToken,
    val deviceIdentity: BadgeUsbDeviceIdentity,
    val selectionStamp: Long,
    val reconnectOperation: ActiveBadgeUsbReconnectOperation?,
    val reconnectAttempt: ActiveBadgeUsbReconnectAttempt?,
) {
    val dispatchGate = BadgeUsbPermissionDispatchGate()
}

private data class PreparedBadgeUsbPermissionRequest(
    val operation: ActiveBadgeUsbPermissionOperation,
    val shouldDispatch: Boolean,
)

private data class ConsumedBadgeUsbPermissionResult(
    val operation: ActiveBadgeUsbPermissionOperation,
    val selectionSnapshot: BadgeUsbSelectionSnapshot,
    val granted: Boolean,
)

private data class BadgeUsbDetachContext(
    val lifecycleSession: Long,
    val reconnectOperation: ActiveBadgeUsbReconnectOperation?,
    val reconnectAttempt: ActiveBadgeUsbReconnectAttempt?,
    val invalidation: BadgeUsbAttachmentInvalidation?,
)

private data class BadgeUsbSelectionSnapshot(
    val stamp: Long,
    val lifecycleSession: Long,
    val lifecycleActive: Boolean,
    val operation: ActiveBadgeUsbReconnectOperation?,
    val operationGeneration: Long?,
    val operationActive: Boolean,
    val verifiedOwner: BadgeUsbOwnerKey?,
    val selectedAttachmentToken: BadgeUsbAttachmentToken?,
    val activeAttachmentToken: BadgeUsbAttachmentToken?,
    val activeConnection: Any?,
    val activeUsbLifecycleSession: Long?,
    val activeUsbIoSession: BadgeUsbIoSession?,
)

private data class BadgeUsbIoSession(
    val lifecycleSession: Long,
    val attachmentToken: BadgeUsbAttachmentToken,
    val connection: android.hardware.usb.UsbDeviceConnection,
    val usbInterface: UsbInterface,
    val inEndpoint: UsbEndpoint,
    val outEndpoint: UsbEndpoint,
)

private data class BadgeUsbEnumerationSnapshot(
    val selectionSnapshot: BadgeUsbSelectionSnapshot,
    val enumerationEpoch: Long,
)

private data class DetachedBadgeUsbInvestigation(
    val operation: ActiveBadgeInvestigation,
    val result: BleInvestigationResult,
)

private data class PreparedBadgeUsbInvestigationFrame(
    val handled: Boolean = false,
    val controlAck: Pair<CompletableDeferred<BadgeControlAck>, BadgeControlAck>? = null,
    val terminal: Pair<CompletableDeferred<BleInvestigationResult>, BleInvestigationResult>? = null,
)

private data class DetachedBadgeUsbResources(
    val connection: android.hardware.usb.UsbDeviceConnection?,
    val usbInterface: UsbInterface?,
    val readJob: Job?,
    val handshakeJob: Job?,
    val statusPollJob: Job?,
    val investigation: DetachedBadgeUsbInvestigation?,
    val ioDrain: BadgeUsbIoArbiter.Drain?,
)

private data class RetainedBadgeUsbDrainCleanup(
    val detached: DetachedBadgeUsbResources,
    val phaseGate: BadgeUsbIoCleanupPhaseGate = BadgeUsbIoCleanupPhaseGate(),
)

private data class PendingBadgeUsbSelection(
    val device: UsbDevice,
    val attachmentToken: BadgeUsbAttachmentToken,
    val reconnectOperation: ActiveBadgeUsbReconnectOperation?,
    val reconnectAttempt: ActiveBadgeUsbReconnectAttempt?,
    val selectionSnapshot: BadgeUsbSelectionSnapshot,
    val enumerationEpoch: Long,
)

private data class PreparedBadgeUsbReconnect(
    val operation: ActiveBadgeUsbReconnectOperation,
    val previousOperation: ActiveBadgeUsbReconnectOperation?,
)

private data class PreparedBadgeUsbStatusPoller(
    val job: Job,
    val previousJob: Job?,
)

private data class PreparedBadgeUsbReader(
    val job: Job,
    val previousJob: Job?,
    val connection: android.hardware.usb.UsbDeviceConnection,
    val lifecycleSession: Long,
    val attachmentToken: BadgeUsbAttachmentToken,
    val ioSession: BadgeUsbIoSession,
    val selectionStamp: Long,
)

private class ActiveBadgeUsbReconnectOperation(
    val ticket: BadgeUsbReconnectTicket,
) {
    lateinit var job: Job
    private val operationGate = BadgeUsbReconnectOperationGate()
    private var attempt: ActiveBadgeUsbReconnectAttempt? = null

    fun preparePendingAttempt(
        lifecycleSession: Long,
        candidateIdentity: BadgeUsbDeviceIdentity,
        operationIsCurrent: () -> Boolean,
        selectAttachment: () -> BadgeUsbAttachmentToken,
    ): ActiveBadgeUsbReconnectAttempt? = operationGate.prepareIfActive {
        synchronized(this) {
            if (!operationIsCurrent()) return@synchronized null
            val current = attempt
            if (current != null &&
                (current.ticket !== ticket || current.lifecycleSession != lifecycleSession)
            ) {
                return@synchronized null
            }
            when (badgeUsbReconnectCandidatePreparation(
                operationActive = true,
                attemptIdentity = current?.attachmentToken?.identity,
                attemptConnectionBound = current?.connectionIdentity != null,
                candidateIdentity = candidateIdentity,
            )) {
                BadgeUsbReconnectCandidatePreparation.REJECT_BEFORE_SELECTION -> null
                BadgeUsbReconnectCandidatePreparation.REUSE_ATTEMPT -> current
                BadgeUsbReconnectCandidatePreparation.SELECT_AND_BIND ->
                    ActiveBadgeUsbReconnectAttempt(
                        ticket = ticket,
                        lifecycleSession = lifecycleSession,
                        attachmentToken = selectAttachment(),
                    ).also { attempt = it }
            }
        }
    }

    fun bindConnection(
        expectedAttempt: ActiveBadgeUsbReconnectAttempt,
        connectionIdentity: Any,
    ): ActiveBadgeUsbReconnectAttempt? = operationGate.prepareIfActive {
        synchronized(this) {
            if (attempt !== expectedAttempt || expectedAttempt.connectionIdentity != null) {
                return@synchronized null
            }
            expectedAttempt.copy(connectionIdentity = connectionIdentity).also { attempt = it }
        }
    }

    fun isActive(): Boolean = operationGate.isActive()

    fun tryTerminalize(): Boolean = operationGate.tryTerminalize()

    fun publishConnectingIfActive(publication: () -> Unit): Boolean =
        operationGate.publishConnectingIfActive(publication)

    fun completeHandshakeAndClearIfActive(
        operationIsCurrent: () -> Boolean,
        fullCommit: () -> Boolean,
        completion: () -> Unit,
    ): Boolean = operationGate.completeHandshakeAndClearIfActive(
        operationIsCurrent = operationIsCurrent,
        fullCommit = fullCommit,
        completion = completion,
    )

    @Synchronized
    fun currentAttempt(
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
    ): ActiveBadgeUsbReconnectAttempt? = attempt?.takeIf {
        it.ticket === ticket &&
            it.lifecycleSession == lifecycleSession &&
            it.attachmentToken == attachmentToken
    }

    @Synchronized
    fun currentAttempt(): ActiveBadgeUsbReconnectAttempt? = attempt

    @Synchronized
    fun ownsAttempt(expectedAttempt: ActiveBadgeUsbReconnectAttempt): Boolean =
        attempt === expectedAttempt && expectedAttempt.ticket === ticket

    @Synchronized
    fun clearAttempt(
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        connectionIdentity: Any?,
    ): Boolean {
        val current = attempt ?: return false
        if (current.ticket !== ticket || current.lifecycleSession != lifecycleSession ||
            current.attachmentToken != attachmentToken ||
            (connectionIdentity != null && current.connectionIdentity !== connectionIdentity)
        ) {
            return false
        }
        attempt = null
        return true
    }

    @Synchronized
    fun clearAttempt() {
        attempt = null
    }
}

@Singleton
class BadgeUsbRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val usbManager: UsbManager,
    okHttpClient: OkHttpClient
) {
    companion object {
        private const val TAG = "BadgeUsbRepository"
        private const val ACTION_USB_PERMISSION = "com.friendorfoe.action.USB_BADGE_PERMISSION"
        private const val EXTRA_USB_PERMISSION_SESSION = "usb_permission_session"
        private const val EXTRA_USB_PERMISSION_ATTACHMENT_GENERATION =
            "usb_permission_attachment_generation"
        private const val EXTRA_USB_PERMISSION_DEVICE_ID = "usb_permission_device_id"
        private const val EXTRA_USB_PERMISSION_DEVICE_PATH = "usb_permission_device_path"
        private const val EXTRA_USB_RECONNECT_GENERATION = "usb_reconnect_generation"
        private const val EXTRA_USB_SELECTION_STAMP = "usb_selection_stamp"
        private const val EXTRA_USB_PERMISSION_GENERATION = "usb_permission_generation"
        private const val NO_LIFECYCLE_SESSION = -1L
        private const val NO_ATTACHMENT_GENERATION = -1L
        private const val NO_RECONNECT_GENERATION = -1L
        private const val NO_SELECTION_STAMP = -1L
        private const val NO_PERMISSION_GENERATION = -1L
        private const val NO_USB_DEVICE_ID = -1
        private const val ESPRESSIF_VENDOR_ID = 0x303A
        private const val BADGE_AP_BASE_URL = "http://192.168.4.1"
        private const val DEBUG_BRIDGE_BASE_URL = "http://10.0.2.2:8765"
        private const val READ_TIMEOUT_MS = 250
        private const val WRITE_TIMEOUT_MS = 250
        private const val USB_IO_DRAIN_TIMEOUT_MS = 2_000L
        private const val USB_IO_CLEANUP_RETRY_MS = 100L
        private const val AP_POLL_INTERVAL_MS = 2500L
        private const val DEBUG_BRIDGE_POLL_INTERVAL_MS = 1500L
        private const val BLE_SCAN_INTERVAL_MS = 6000L
        private const val BLE_SCAN_WINDOW_MS = 4500L
        private const val USB_STATUS_POLL_INTERVAL_MS = 2000L
        private const val USB_IDENTITY_HANDSHAKE_TIMEOUT_MS = 15_000L
        private const val USB_IDENTITY_HANDSHAKE_RETRY_MS = 1_000L
        // A retry writes PING and STATUS; each bulk transfer is bounded by WRITE_TIMEOUT_MS.
        private const val USB_IDENTITY_HANDSHAKE_WRITE_BUDGET_MS = WRITE_TIMEOUT_MS * 2L
        private const val MAX_RECENT_DETECTIONS = 20
        private const val BADGE_CONTROL_ACK_TIMEOUT_MS = 1500L
        private const val BADGE_CHUNK_RETRY_MS = 100L
        private const val BADGE_MAX_INVESTIGATION_CHUNKS = 64
        private const val MAX_BADGE_STATUS_BODY_BYTES = 64 * 1024
        private const val MAX_USED_INVESTIGATION_IDS = 32
        private const val GATT_OPERATION_TIMEOUT = 0x100
        private const val GATT_OPERATION_DISCONNECTED = 0x101
        private val BADGE_BLE_SERVICE_UUID: UUID =
            UUID.fromString("0000f0f0-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_STATUS_UUID: UUID =
            UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_CONTROL_UUID: UUID =
            UUID.fromString("0000ff02-0000-1000-8000-00805f9b34fb")
        private val BADGE_BLE_INVESTIGATION_UUID: UUID =
            UUID.fromString("0000ff03-0000-1000-8000-00805f9b34fb")
        private val CLIENT_CONFIG_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val connectionMutex = Mutex()
    private val usbWriteMutex = Mutex()
    private val bleGattOperationMutex = Mutex()
    private val investigationLock = Any()
    private val badgeHttpClient = okHttpClient.newBuilder()
        .connectTimeout(1200, TimeUnit.MILLISECONDS)
        .readTimeout(1200, TimeUnit.MILLISECONDS)
        .writeTimeout(1200, TimeUnit.MILLISECONDS)
        .build()
    private val jsonMediaType = "application/json".toMediaType()

    private val _state = MutableStateFlow(BadgeUsbState())
    val state: StateFlow<BadgeUsbState> = _state.asStateFlow()
    private val _investigation = MutableStateFlow<BleInvestigationResult?>(null)
    val investigation: StateFlow<BleInvestigationResult?> = _investigation.asStateFlow()

    private val lifecycleGate = BadgeUsbLifecycleGate()
    private val attachmentGate = BadgeUsbAttachmentGate()
    private val usbStatusPollGate = BadgeUsbStatusPollGate()
    private val usbLiteLiveGate = BadgeUsbLiteLiveGate()
    private val usbReconnectGate = BadgeUsbReconnectGate()
    private val usbReconnectOperationSlot = BadgeUsbAtomicSlot<ActiveBadgeUsbReconnectOperation>()
    private val usbReconnectSelectionGate = BadgeUsbReconnectSelectionGate()
    private val usbEnumerationEpochGate = BadgeUsbEnumerationEpochGate()
    private val usbIoArbiter = BadgeUsbIoArbiter()
    private val receiverLifetimeGate = BadgeUsbReceiverLifetimeGate()
    private val lateUsbCleanupSlot = BadgeUsbRetainedCleanupSlot()
    private var nextUsbPermissionGeneration = 1L
    private var activeUsbPermissionOperation: ActiveBadgeUsbPermissionOperation? = null
    private var readJob: Job? = null
    private var apPollJob: Job? = null
    private var debugBridgePollJob: Job? = null
    private var blePollJob: Job? = null
    private val usbStatusPollJobSlot = BadgeUsbAtomicSlot<Job>()
    @Volatile private var usbHandshakeJob: Job? = null
    @Volatile private var activeConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var activeInterface: UsbInterface? = null
    @Volatile private var activeOutEndpoint: UsbEndpoint? = null
    @Volatile private var activeUsbLifecycleSession: Long? = null
    @Volatile private var activeAttachmentToken: BadgeUsbAttachmentToken? = null
    @Volatile private var verifiedUsbOwnerKey: BadgeUsbOwnerKey? = null
    @Volatile private var activeUsbIoSession: BadgeUsbIoSession? = null
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager?.adapter
    @Volatile private var activeGatt: BluetoothGatt? = null
    @Volatile private var activeBleControlChar: BluetoothGattCharacteristic? = null
    @Volatile private var activeBleStatusChar: BluetoothGattCharacteristic? = null
    @Volatile private var activeBleInvestigationChar: BluetoothGattCharacteristic? = null
    @Volatile private var pendingBleGattOperation: PendingBadgeGattOperation? = null
    @Volatile private var bleScanning = false
    private var investigationGeneration = 0L
    private var activeInvestigation: ActiveBadgeInvestigation? = null
    private val usedInvestigationRequestIds = LinkedHashSet<String>()

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device = intent.usbDeviceExtra() ?: return
                    val lifecycleSession = intent.getLongExtra(
                        EXTRA_USB_PERMISSION_SESSION,
                        NO_LIFECYCLE_SESSION,
                    )
                    val attachmentToken = intent.usbAttachmentToken() ?: return
                    val identity = device.attachmentIdentity()
                    val reconnectGeneration = intent.getLongExtra(
                        EXTRA_USB_RECONNECT_GENERATION,
                        NO_RECONNECT_GENERATION,
                    )
                    val selectionStamp = intent.getLongExtra(
                        EXTRA_USB_SELECTION_STAMP,
                        NO_SELECTION_STAMP,
                    )
                    val permissionGeneration = intent.getLongExtra(
                        EXTRA_USB_PERMISSION_GENERATION,
                        NO_PERMISSION_GENERATION,
                    )
                    val permissionGranted = intent.getBooleanExtra(
                        UsbManager.EXTRA_PERMISSION_GRANTED,
                        false,
                    )
                    val consumed = usbReconnectSelectionGate.withBarrier {
                        val operation = activeUsbPermissionOperation
                            ?: return@withBarrier null
                        if (permissionGeneration == NO_PERMISSION_GENERATION ||
                            operation.generation != permissionGeneration ||
                            operation.selectionStamp != selectionStamp ||
                            selectionStamp == NO_SELECTION_STAMP ||
                            !usbReconnectSelectionGate.isStampCurrent(selectionStamp) ||
                            operation.lifecycleSession != lifecycleSession ||
                            operation.attachmentToken != attachmentToken ||
                            operation.deviceIdentity != identity ||
                            !lifecycleGate.isActive(lifecycleSession) ||
                            !attachmentGate.acceptsPermission(attachmentToken, identity)
                        ) {
                            return@withBarrier null
                        }
                        val exactReconnectContext = if (
                            reconnectGeneration != NO_RECONNECT_GENERATION
                        ) {
                            val reconnectOperation = operation.reconnectOperation
                                ?: return@withBarrier null
                            val reconnectAttempt = operation.reconnectAttempt
                                ?: return@withBarrier null
                            if (reconnectOperation.ticket.generation != reconnectGeneration ||
                                !isUsbReconnectAttemptCurrent(
                                    operation = reconnectOperation,
                                    attempt = reconnectAttempt,
                                    deviceIdentity = identity,
                                    lifecycleSession = lifecycleSession,
                                    attachmentToken = attachmentToken,
                                    expectedConnectionIdentity = null,
                                )
                            ) {
                                return@withBarrier null
                            }
                            reconnectOperation to reconnectAttempt
                        } else {
                            if (operation.reconnectOperation != null ||
                                operation.reconnectAttempt != null ||
                                usbReconnectOperationSlot.current() != null
                            ) {
                                return@withBarrier null
                            }
                            null
                        }
                        if (!clearExactUsbPermissionOperationLocked(operation)) {
                            return@withBarrier null
                        }
                        usbReconnectSelectionGate.advanceStamp()
                        val postConsumptionSnapshot =
                            captureUsbSelectionSnapshotLocked(lifecycleSession)
                        if (!permissionGranted) {
                            val denialStillOwnsSelection =
                                attachmentGate.acceptsPermission(attachmentToken, identity) &&
                                    verifiedUsbOwnerKey == null && activeConnection == null &&
                                    activeAttachmentToken == null
                            if (denialStillOwnsSelection) {
                                setState {
                                    it.copy(
                                        status = BadgeUsbStatus.PERMISSION_NEEDED,
                                        deviceName = device.displayName(),
                                        message = "USB permission denied",
                                        transportLabel = "USB-C",
                                    )
                                }
                            }
                        }
                        ConsumedBadgeUsbPermissionResult(
                            operation = operation.copy(
                                reconnectOperation = exactReconnectContext?.first,
                                reconnectAttempt = exactReconnectContext?.second,
                            ),
                            selectionSnapshot = postConsumptionSnapshot,
                            granted = permissionGranted,
                        )
                    } ?: return
                    if (consumed.granted) {
                        scope.launch {
                            connectToDevice(
                                device = device,
                                lifecycleSession = lifecycleSession,
                                attachmentToken = attachmentToken,
                                reconnectOperation = consumed.operation.reconnectOperation,
                                reconnectAttempt = consumed.operation.reconnectAttempt,
                                selectionSnapshot = consumed.selectionSnapshot,
                            )
                        }
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> requestConnection()
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val detached = intent.usbDeviceExtra()
                    if (detached != null && detached.vendorId == ESPRESSIF_VENDOR_ID) {
                        val detachedIdentity = detached.attachmentIdentity()
                        val detachContext = usbReconnectSelectionGate.withBarrier {
                            // A detach must invalidate every pre-detach device-list read,
                            // even across a stop/start lifecycle handoff.
                            usbEnumerationEpochGate.advanceEpoch()
                            val lifecycleSession = lifecycleGate.activeSession()
                                ?: return@withBarrier null
                            activeUsbIoSession?.takeIf {
                                it.attachmentToken.identity == detachedIdentity
                            }?.let { usbIoArbiter.revoke(it) }
                            val permissionCleared = activeUsbPermissionOperation
                                ?.takeIf { it.deviceIdentity == detachedIdentity }
                                ?.let(::clearExactUsbPermissionOperationLocked) == true
                            val reconnectOperation = usbReconnectOperationSlot.current()?.takeIf {
                                usbReconnectGate.isCurrent(it.ticket, it.ticket.lifecycleSession)
                            }
                            val reconnectAttempt = reconnectOperation?.currentAttempt()
                            val reconnectDetachMatches = reconnectOperation != null &&
                                (badgeUsbReconnectDetachMatches(
                                    reconnectOperation.ticket,
                                    detachedIdentity,
                                ) || reconnectAttempt?.attachmentToken?.identity == detachedIdentity)
                            val terminalized = reconnectDetachMatches &&
                                reconnectOperation?.tryTerminalize() == true
                            val invalidation = attachmentGate.invalidateMatching(detachedIdentity)
                            if (terminalized || invalidation != null || permissionCleared) {
                                usbReconnectSelectionGate.advanceStamp()
                            }
                            BadgeUsbDetachContext(
                                lifecycleSession = lifecycleSession,
                                reconnectOperation = reconnectOperation,
                                reconnectAttempt = reconnectAttempt,
                                invalidation = invalidation,
                            )
                        } ?: return
                        val lifecycleSession = detachContext.lifecycleSession
                        val reconnectOperation = detachContext.reconnectOperation
                        val reconnectAttempt = detachContext.reconnectAttempt
                        val invalidation = detachContext.invalidation
                        val reconnectDetachMatches = reconnectOperation != null &&
                            (badgeUsbReconnectDetachMatches(
                                reconnectOperation.ticket,
                                detachedIdentity,
                            ) || reconnectAttempt?.attachmentToken?.identity == detachedIdentity)
                        if (reconnectDetachMatches && reconnectOperation != null) {
                            cancelUsbReconnectForDetachedOwner(
                                operation = reconnectOperation,
                                reconnectAttemptSnapshot = reconnectAttempt,
                                detachedInvalidation = invalidation,
                                lifecycleSession = lifecycleSession,
                            )
                        } else {
                            cleanupUsbDetachAndRescan(invalidation, lifecycleSession)
                        }
                    }
                }
            }
        }
    }

    fun start() {
        val lifecycleSession = usbReconnectSelectionGate.withBarrier {
            if (!lifecycleGate.begin()) return@withBarrier null
            usbReconnectSelectionGate.advanceStamp()
            lifecycleGate.activeSession()
        } ?: return
        registerReceiverIfNeeded()
        requestConnection(lifecycleSession)
        if (BadgeControlTransportPolicy.allowsBleTether()) {
            startBlePoller()
        }
        if (BadgeControlTransportPolicy.allowsReadOnlyHttpStatus()) {
            startApPoller()
            startDebugBridgePoller()
        }
    }

    fun stop() {
        val lifecycleSession = lifecycleGate.activeSession() ?: return
        var cleanupSnapshot: BadgeUsbSelectionSnapshot? = null
        val stopped = usbReconnectSelectionGate.withBarrier {
            cleanupSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession)
            cleanupSnapshot?.activeUsbIoSession?.let { usbIoArbiter.revoke(it) }
            val permissionCleared = activeUsbPermissionOperation
                ?.takeIf { it.lifecycleSession == lifecycleSession }
                ?.let(::clearExactUsbPermissionOperationLocked) == true
            val operationTerminalized = usbReconnectOperationSlot.current()
                ?.takeIf { it.ticket.lifecycleSession == lifecycleSession }
                ?.tryTerminalize() == true
            if (!lifecycleGate.end(lifecycleSession)) {
                false
            } else {
                val invalidation = attachmentGate.invalidateCurrent()
                if (operationTerminalized || permissionCleared ||
                    invalidation != null || cleanupSnapshot != null
                ) {
                    usbReconnectSelectionGate.advanceStamp()
                }
                true
            }
        }
        if (!stopped) return
        cancelUsbReconnectForLifecycle(lifecycleSession)
        disconnect(
            reason = "Badge USB stopped",
            lifecycleSession = lifecycleSession,
            expectedSnapshot = cleanupSnapshot,
        )
        apPollJob?.cancel()
        apPollJob = null
        debugBridgePollJob?.cancel()
        debugBridgePollJob = null
        blePollJob?.cancel()
        blePollJob = null
        closeBle("Badge BLE stopped")
    }

    fun refresh() {
        val lifecycleSession = lifecycleGate.activeSession() ?: return
        refresh(lifecycleSession)
    }

    private fun refresh(lifecycleSession: Long) {
        val enumerationSnapshot = captureUsbEnumerationSnapshot(lifecycleSession) ?: return
        val candidates = findBadgeCandidates()
        var reconnectTicket: BadgeUsbReconnectTicket? = null
        val pendingSelection = usbReconnectSelectionGate.withBarrier {
            if (!usbEnumerationSnapshotIsCurrent(enumerationSnapshot)) return@withBarrier null
            val rawOperation = usbReconnectOperationSlot.current()
            val currentOperation = currentUsbReconnectOperation(lifecycleSession)
            if (rawOperation != null && currentOperation == null) return@withBarrier null
            if (currentOperation != null) {
                usbEnumerationEpochGate.advanceEpoch()
                reconnectTicket = currentOperation.ticket
                return@withBarrier null
            }
            if (!genericUsbSelectionMayMutate(lifecycleSession)) return@withBarrier null
            val decisionEpoch = usbEnumerationEpochGate.advanceEpoch()
            if (candidates.isEmpty()) {
                val selectedAttachmentToken = attachmentGate.currentToken()
                val invalidated = attachmentGate.invalidateCurrent()
                val permissionCleared = clearGenericUsbPermissionOperationLocked(
                    lifecycleSession = lifecycleSession,
                    expectedAttachmentToken = selectedAttachmentToken,
                )
                if (invalidated != null || permissionCleared) {
                    usbReconnectSelectionGate.advanceStamp()
                }
                setState { current ->
                    current.copy(
                        status = BadgeUsbStatus.DISCONNECTED,
                        deviceName = null,
                        message = "Attach a FoF badge over USB-C",
                        transportLabel = ""
                    )
                }
                return@withBarrier null
            }
            if (candidates.size > 1) {
                val selectedAttachmentToken = attachmentGate.currentToken()
                val invalidated = attachmentGate.invalidateCurrent()
                val permissionCleared = clearGenericUsbPermissionOperationLocked(
                    lifecycleSession = lifecycleSession,
                    expectedAttachmentToken = selectedAttachmentToken,
                )
                if (invalidated != null || permissionCleared) {
                    usbReconnectSelectionGate.advanceStamp()
                }
                reportAmbiguousBadgeDevices(candidates)
                return@withBarrier null
            }
            val device = candidates.first()
            val previousToken = attachmentGate.currentToken()
            val attachmentToken = selectAttachment(device, lifecycleSession)
            if (attachmentToken != previousToken) {
                activeUsbPermissionOperation?.let(::clearExactUsbPermissionOperationLocked)
                usbReconnectSelectionGate.advanceStamp()
            }
            PendingBadgeUsbSelection(
                device = device,
                attachmentToken = attachmentToken,
                reconnectOperation = null,
                reconnectAttempt = null,
                selectionSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession),
                enumerationEpoch = decisionEpoch,
            )
        }

        reconnectTicket?.let { ticket ->
            requestConnection(
                lifecycleSession = lifecycleSession,
                preserveRecoveryOnNoCandidates = true,
                reconnectTicket = ticket,
            )
            return
        }
        val selection = pendingSelection ?: return
        val hasPermission = usbManager.hasPermission(selection.device)
        val selectionStillCurrent = usbReconnectSelectionGate.withBarrier {
            usbEnumerationEpochGate.isEpochCurrent(selection.enumerationEpoch) &&
                usbSelectionSnapshotIsCurrent(selection.selectionSnapshot) &&
                attachmentGate.isCurrent(selection.attachmentToken)
        }
        if (!selectionStillCurrent) return
        if (!hasPermission) {
            usbReconnectSelectionGate.withBarrier {
                if (!usbEnumerationEpochGate.isEpochCurrent(selection.enumerationEpoch) ||
                    !usbSelectionSnapshotIsCurrent(selection.selectionSnapshot)
                ) {
                    return@withBarrier
                }
                setState {
                    it.copy(
                        status = BadgeUsbStatus.PERMISSION_NEEDED,
                        deviceName = selection.device.displayName(),
                        message = "FoF badge found. USB access required.",
                        transportLabel = "USB-C"
                    )
                }
            }
            return
        }
        scope.launch {
            connectToDevice(
                device = selection.device,
                lifecycleSession = lifecycleSession,
                attachmentToken = selection.attachmentToken,
                selectionSnapshot = selection.selectionSnapshot,
            )
        }
    }

    fun requestConnection() {
        val lifecycleSession = lifecycleGate.activeSession() ?: return
        requestConnection(lifecycleSession)
    }

    private fun requestConnection(
        lifecycleSession: Long,
        preserveRecoveryOnNoCandidates: Boolean = false,
        reconnectTicket: BadgeUsbReconnectTicket? = null,
    ) {
        val enumerationSnapshot = captureUsbEnumerationSnapshot(lifecycleSession) ?: return
        val candidates = findBadgeCandidates()
        var ambiguousOperation: ActiveBadgeUsbReconnectOperation? = null
        val pendingSelection = usbReconnectSelectionGate.withBarrier {
            if (!usbEnumerationSnapshotIsCurrent(enumerationSnapshot)) return@withBarrier null
            val rawOperation = usbReconnectOperationSlot.current()
            val requestedReconnectOperation = reconnectTicket?.let { requestedTicket ->
                currentUsbReconnectOperation(lifecycleSession)?.takeIf {
                    it === rawOperation && it.ticket === requestedTicket
                }
            }
            if (reconnectTicket != null && requestedReconnectOperation == null) {
                return@withBarrier null
            }
            val effectiveOperation = requestedReconnectOperation
                ?: currentUsbReconnectOperation(lifecycleSession)
            if (rawOperation != null && effectiveOperation == null) return@withBarrier null
            if (effectiveOperation == null && !genericUsbSelectionMayMutate(lifecycleSession)) {
                return@withBarrier null
            }
            val decisionEpoch = usbEnumerationEpochGate.advanceEpoch()

            when (badgeUsbReconnectCandidateAction(
                candidateCount = candidates.size,
                preserveRecovery = preserveRecoveryOnNoCandidates ||
                    effectiveOperation != null,
            )) {
                BadgeUsbReconnectCandidateAction.PRESERVE_RECOVERY -> return@withBarrier null
                BadgeUsbReconnectCandidateAction.NORMAL_REFRESH -> {
                    if (genericUsbSelectionMayMutate(lifecycleSession)) {
                        val selectedAttachmentToken = attachmentGate.currentToken()
                        val invalidated = attachmentGate.invalidateCurrent()
                        val permissionCleared = clearGenericUsbPermissionOperationLocked(
                            lifecycleSession = lifecycleSession,
                            expectedAttachmentToken = selectedAttachmentToken,
                        )
                        if (invalidated != null || permissionCleared) {
                            usbReconnectSelectionGate.advanceStamp()
                        }
                        setState { current ->
                            current.copy(
                                status = BadgeUsbStatus.DISCONNECTED,
                                deviceName = null,
                                message = "Attach a FoF badge over USB-C",
                                transportLabel = ""
                            )
                        }
                    }
                    return@withBarrier null
                }
                BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS -> {
                    if (effectiveOperation != null && effectiveOperation.tryTerminalize()) {
                        clearReconnectUsbPermissionOperationLocked(
                            lifecycleSession = lifecycleSession,
                            expectedReconnectOperation = effectiveOperation,
                        )
                        ambiguousOperation = effectiveOperation
                        usbReconnectSelectionGate.advanceStamp()
                    } else if (effectiveOperation == null &&
                        genericUsbSelectionMayMutate(lifecycleSession)
                    ) {
                        val selectedAttachmentToken = attachmentGate.currentToken()
                        val invalidated = attachmentGate.invalidateCurrent()
                        val permissionCleared = clearGenericUsbPermissionOperationLocked(
                            lifecycleSession = lifecycleSession,
                            expectedAttachmentToken = selectedAttachmentToken,
                        )
                        if (invalidated != null || permissionCleared) {
                            usbReconnectSelectionGate.advanceStamp()
                        }
                        reportAmbiguousBadgeDevices(candidates)
                    }
                    return@withBarrier null
                }
                BadgeUsbReconnectCandidateAction.CONNECT_ONE -> Unit
            }
            val device = candidates.first()
            val deviceIdentity = device.attachmentIdentity()
            val previousToken = attachmentGate.currentToken()
            val previousAttempt = effectiveOperation?.currentAttempt()
            val preparedAttempt = if (effectiveOperation != null) {
                val attempt = effectiveOperation.preparePendingAttempt(
                    lifecycleSession = lifecycleSession,
                    candidateIdentity = deviceIdentity,
                    operationIsCurrent = {
                        usbReconnectOperationSlot.current() === effectiveOperation &&
                            usbReconnectGate.isCurrent(
                                effectiveOperation.ticket,
                                lifecycleSession,
                            ) && lifecycleGate.isActive(lifecycleSession)
                    },
                    selectAttachment = { selectAttachment(device, lifecycleSession) },
                ) ?: return@withBarrier null
                attempt.attachmentToken to attempt
            } else {
                if (!genericUsbSelectionMayMutate(lifecycleSession)) return@withBarrier null
                selectAttachment(device, lifecycleSession) to null
            }
            if (preparedAttempt.first != previousToken || preparedAttempt.second !== previousAttempt) {
                activeUsbPermissionOperation?.let(::clearExactUsbPermissionOperationLocked)
                usbReconnectSelectionGate.advanceStamp()
            }
            PendingBadgeUsbSelection(
                device = device,
                attachmentToken = preparedAttempt.first,
                reconnectOperation = effectiveOperation,
                reconnectAttempt = preparedAttempt.second,
                selectionSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession),
                enumerationEpoch = decisionEpoch,
            )
        }
        ambiguousOperation?.let {
            failAmbiguousUsbReconnect(it, candidates)
            return
        }
        val selection = pendingSelection ?: return
        val device = selection.device
        val attachmentToken = selection.attachmentToken
        val effectiveReconnectOperation = selection.reconnectOperation
        val reconnectAttempt = selection.reconnectAttempt
        val deviceIdentity = device.attachmentIdentity()
        if (effectiveReconnectOperation != null &&
            (reconnectAttempt == null || !isUsbReconnectAttemptCurrent(
                operation = effectiveReconnectOperation,
                attempt = reconnectAttempt,
                deviceIdentity = deviceIdentity,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                expectedConnectionIdentity = reconnectAttempt.connectionIdentity,
            ))
        ) {
            return
        }
        val hasPermission = usbManager.hasPermission(device)
        val selectionStillCurrent = usbReconnectSelectionGate.withBarrier {
            usbEnumerationEpochGate.isEpochCurrent(selection.enumerationEpoch) &&
                usbSelectionSnapshotIsCurrent(selection.selectionSnapshot) &&
                attachmentGate.isCurrent(attachmentToken) &&
                (effectiveReconnectOperation == null ||
                    (reconnectAttempt != null && isUsbReconnectAttemptCurrent(
                        operation = effectiveReconnectOperation,
                        attempt = reconnectAttempt,
                        deviceIdentity = deviceIdentity,
                        lifecycleSession = lifecycleSession,
                        attachmentToken = attachmentToken,
                        expectedConnectionIdentity = null,
                    )))
        }
        if (!selectionStillCurrent) return
        if (hasPermission) {
            val connectSelectionSnapshot = usbReconnectSelectionGate.withBarrier {
                if (!usbEnumerationEpochGate.isEpochCurrent(selection.enumerationEpoch) ||
                    !usbSelectionSnapshotIsCurrent(selection.selectionSnapshot) ||
                    !attachmentGate.isCurrent(attachmentToken)
                ) {
                    return@withBarrier null
                }
                val permissionCleared = activeUsbPermissionOperation
                    ?.let(::clearExactUsbPermissionOperationLocked) == true
                if (permissionCleared) {
                    usbReconnectSelectionGate.advanceStamp()
                }
                captureUsbSelectionSnapshotLocked(lifecycleSession)
            } ?: return
            scope.launch {
                connectToDevice(
                    device = device,
                    lifecycleSession = lifecycleSession,
                    attachmentToken = attachmentToken,
                    reconnectOperation = effectiveReconnectOperation,
                    reconnectAttempt = reconnectAttempt,
                    selectionSnapshot = connectSelectionSnapshot,
                )
            }
            return
        }

        val preparedPermission = usbReconnectSelectionGate.withBarrier {
            if (!usbEnumerationEpochGate.isEpochCurrent(selection.enumerationEpoch) ||
                !usbSelectionSnapshotIsCurrent(selection.selectionSnapshot) ||
                !attachmentGate.isCurrent(attachmentToken)
            ) {
                return@withBarrier null
            }
            val reconnectStillCurrent = if (
                effectiveReconnectOperation != null && reconnectAttempt != null
            ) {
                isUsbReconnectAttemptCurrent(
                    operation = effectiveReconnectOperation,
                    attempt = reconnectAttempt,
                    deviceIdentity = deviceIdentity,
                    lifecycleSession = lifecycleSession,
                    attachmentToken = attachmentToken,
                    expectedConnectionIdentity = null,
                )
            } else {
                effectiveReconnectOperation == null &&
                    genericUsbSelectionMayMutate(lifecycleSession)
            }
            if (!reconnectStillCurrent) return@withBarrier null
            val existing = activeUsbPermissionOperation
            if (existing != null &&
                existing.lifecycleSession == lifecycleSession &&
                existing.attachmentToken == attachmentToken &&
                existing.deviceIdentity == deviceIdentity &&
                existing.selectionStamp == selection.selectionSnapshot.stamp &&
                existing.reconnectOperation === effectiveReconnectOperation &&
                existing.reconnectAttempt === reconnectAttempt
            ) {
                return@withBarrier PreparedBadgeUsbPermissionRequest(
                    operation = existing,
                    shouldDispatch = false,
                )
            }
            check(nextUsbPermissionGeneration != Long.MAX_VALUE) {
                "USB permission generation exhausted"
            }
            val permissionSelectionStamp = usbReconnectSelectionGate.advanceStamp()
            val permissionOperation = ActiveBadgeUsbPermissionOperation(
                generation = nextUsbPermissionGeneration++,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                deviceIdentity = deviceIdentity,
                selectionStamp = permissionSelectionStamp,
                reconnectOperation = effectiveReconnectOperation,
                reconnectAttempt = reconnectAttempt,
            )
            replaceUsbPermissionOperationLocked(permissionOperation)
            setState { current ->
                current.copy(
                    status = BadgeUsbStatus.PERMISSION_NEEDED,
                    deviceName = device.displayName(),
                    message = "Waiting for USB permission",
                    transportLabel = "USB-C",
                )
            }
            PreparedBadgeUsbPermissionRequest(
                operation = permissionOperation,
                shouldDispatch = true,
            )
        } ?: return
        if (!preparedPermission.shouldDispatch) return
        val permissionOperation = preparedPermission.operation

        try {
            // UsbManager supplies EXTRA_DEVICE and EXTRA_PERMISSION_GRANTED through a fill-in
            // intent. Android 12+ therefore requires this narrowly scoped, explicit callback
            // PendingIntent to remain mutable.
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    PendingIntent.FLAG_MUTABLE
                } else {
                    0
                }
            val permissionIntent = PendingIntent.getBroadcast(
                context,
                usbPermissionRequestCode(
                    lifecycleSession = lifecycleSession,
                    attachmentToken = attachmentToken,
                    selectionStamp = permissionOperation.selectionStamp,
                    permissionGeneration = permissionOperation.generation,
                    reconnectGeneration = effectiveReconnectOperation?.ticket?.generation
                        ?: NO_RECONNECT_GENERATION,
                ),
                Intent(ACTION_USB_PERMISSION)
                    .setPackage(context.packageName)
                    .setData(Uri.parse(
                        "fof-badge-usb://permission/${permissionOperation.generation}/" +
                            "${permissionOperation.selectionStamp}/" +
                            attachmentToken.generation,
                    ))
                    .putExtra(EXTRA_USB_PERMISSION_SESSION, lifecycleSession)
                    .putExtra(EXTRA_USB_SELECTION_STAMP, permissionOperation.selectionStamp)
                    .putExtra(EXTRA_USB_PERMISSION_GENERATION, permissionOperation.generation)
                    .putExtra(
                        EXTRA_USB_PERMISSION_ATTACHMENT_GENERATION,
                        attachmentToken.generation,
                    )
                    .putExtra(EXTRA_USB_PERMISSION_DEVICE_ID, attachmentToken.identity.deviceId)
                    .putExtra(
                        EXTRA_USB_PERMISSION_DEVICE_PATH,
                        attachmentToken.identity.devicePath,
                    )
                    .putExtra(
                        EXTRA_USB_RECONNECT_GENERATION,
                        effectiveReconnectOperation?.ticket?.generation
                            ?: NO_RECONNECT_GENERATION,
                    ),
                flags,
            )
            val dispatched = usbReconnectSelectionGate.withBarrier {
                if (!usbPermissionOperationMayDispatchLocked(permissionOperation)) {
                    return@withBarrier false
                }
                permissionOperation.dispatchGate.dispatchIfActive {
                    usbManager.requestPermission(device, permissionIntent)
                }
            }
            if (!dispatched) return
        } catch (failure: Exception) {
            failUsbPermissionDispatch(permissionOperation, device, failure)
        }
    }

    // Call only while usbReconnectSelectionGate is held. Its caller keeps that
    // barrier through dispatchIfActive and UsbManager.requestPermission, making
    // the final validation and physical side effect one ordered transaction.
    private fun usbPermissionOperationMayDispatchLocked(
        permissionOperation: ActiveBadgeUsbPermissionOperation,
    ): Boolean {
        val reconnectOwned = if (permissionOperation.reconnectOperation != null &&
            permissionOperation.reconnectAttempt != null
        ) {
            isUsbReconnectAttemptCurrent(
                operation = permissionOperation.reconnectOperation,
                attempt = permissionOperation.reconnectAttempt,
                deviceIdentity = permissionOperation.deviceIdentity,
                lifecycleSession = permissionOperation.lifecycleSession,
                attachmentToken = permissionOperation.attachmentToken,
                expectedConnectionIdentity = null,
            )
        } else {
            permissionOperation.reconnectOperation == null &&
                permissionOperation.reconnectAttempt == null &&
                genericUsbSelectionMayMutate(permissionOperation.lifecycleSession)
        }
        return badgeUsbPermissionMayDispatch(
            activeOperation = activeUsbPermissionOperation,
            expectedOperation = permissionOperation,
            selectionStampCurrent = usbReconnectSelectionGate.isStampCurrent(
                permissionOperation.selectionStamp,
            ),
            lifecycleActive = lifecycleGate.isActive(permissionOperation.lifecycleSession),
            attachmentAccepted = attachmentGate.acceptsPermission(
                permissionOperation.attachmentToken,
                permissionOperation.deviceIdentity,
            ),
            reconnectOwned = reconnectOwned,
        )
    }

    private fun failUsbPermissionDispatch(
        permissionOperation: ActiveBadgeUsbPermissionOperation,
        device: UsbDevice,
        failure: Exception,
    ) {
        usbReconnectSelectionGate.withBarrier {
            if (!clearExactUsbPermissionOperationLocked(permissionOperation)) {
                return@withBarrier
            }
            val reconnectStillOwned = if (permissionOperation.reconnectOperation != null &&
                permissionOperation.reconnectAttempt != null
            ) {
                isUsbReconnectAttemptCurrent(
                    operation = permissionOperation.reconnectOperation,
                    attempt = permissionOperation.reconnectAttempt,
                    deviceIdentity = permissionOperation.deviceIdentity,
                    lifecycleSession = permissionOperation.lifecycleSession,
                    attachmentToken = permissionOperation.attachmentToken,
                    expectedConnectionIdentity = null,
                )
            } else {
                permissionOperation.reconnectOperation == null &&
                    permissionOperation.reconnectAttempt == null &&
                    usbReconnectOperationSlot.current() == null
            }
            val failureStillOwnsState =
                usbReconnectSelectionGate.isStampCurrent(permissionOperation.selectionStamp) &&
                    lifecycleGate.isActive(permissionOperation.lifecycleSession) &&
                    attachmentGate.acceptsPermission(
                        permissionOperation.attachmentToken,
                        permissionOperation.deviceIdentity,
                    ) && reconnectStillOwned && verifiedUsbOwnerKey == null &&
                    activeConnection == null && activeAttachmentToken == null
            usbReconnectSelectionGate.advanceStamp()
            if (failureStillOwnsState) {
                setState { current ->
                    current.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "USB permission request failed",
                        transportLabel = "USB-C",
                    )
                }
            }
        }
        Log.w(TAG, "Badge USB permission request failed", failure)
    }

    // Call only while usbReconnectSelectionGate is held. cancel() is serialized
    // with dispatchIfActive(): it either prevents the platform call or waits for
    // an already-running call to return before ownership is cleared.
    private fun clearExactUsbPermissionOperationLocked(
        expected: ActiveBadgeUsbPermissionOperation,
    ): Boolean {
        if (activeUsbPermissionOperation !== expected) return false
        expected.dispatchGate.cancel()
        activeUsbPermissionOperation = null
        return true
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun replaceUsbPermissionOperationLocked(
        replacement: ActiveBadgeUsbPermissionOperation,
    ) {
        val previous = activeUsbPermissionOperation
        if (previous === replacement) return
        if (previous != null) {
            check(clearExactUsbPermissionOperationLocked(previous)) {
                "Exact badge USB permission replacement lost its predecessor"
            }
        }
        activeUsbPermissionOperation = replacement
    }

    // Call only while usbReconnectSelectionGate is held. Generic enumeration
    // may retire only its exact lifecycle+attachment request and must not consume
    // a reconnect-owned request. A null token is intentionally not a wildcard.
    private fun clearGenericUsbPermissionOperationLocked(
        lifecycleSession: Long,
        expectedAttachmentToken: BadgeUsbAttachmentToken?,
    ): Boolean {
        val permission = activeUsbPermissionOperation ?: return false
        if (!badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = permission.lifecycleSession,
                permissionAttachmentToken = permission.attachmentToken,
                permissionReconnectOperation = permission.reconnectOperation,
                expectedLifecycleSession = lifecycleSession,
                expectedAttachmentToken = expectedAttachmentToken,
            )
        ) {
            return false
        }
        return clearExactUsbPermissionOperationLocked(permission)
    }

    // Call only while usbReconnectSelectionGate is held. A reconnect permission
    // belongs to the operation, not to its mutable current attempt. This lets
    // terminal operation A retire P_A even after attempt B has been prepared.
    private fun clearReconnectUsbPermissionOperationLocked(
        lifecycleSession: Long,
        expectedReconnectOperation: ActiveBadgeUsbReconnectOperation,
    ): Boolean {
        val permission = activeUsbPermissionOperation ?: return false
        if (!badgeUsbReconnectPermissionCleanupMatches(
                permissionLifecycleSession = permission.lifecycleSession,
                permissionReconnectOperation = permission.reconnectOperation,
                expectedLifecycleSession = lifecycleSession,
                expectedReconnectOperation = expectedReconnectOperation,
            )
        ) {
            return false
        }
        return clearExactUsbPermissionOperationLocked(permission)
    }

    fun sendPing() {
        val owner = verifiedUsbOwnerKey ?: return
        scope.launch {
            writeVerifiedUsbLine(
                line = "FOF_PING",
                expectedOwner = owner,
            )
        }
    }

    fun requestStatus() {
        val expectedOwner = verifiedUsbOwnerKey
        scope.launch {
            if (expectedOwner != null && hasUsbCommandPath(expectedOwner)) {
                writeVerifiedUsbLine(
                    line = "FOF_STATUS",
                    expectedOwner = expectedOwner,
                )
            } else {
                fetchNetworkStatus(showErrors = true)
            }
        }
    }

    fun investigateBle(request: com.friendorfoe.detection.BleInvestigationRequest): Boolean {
        val usbOwnerSnapshot = verifiedUsbOwnerKey
        val command = badgeInvestigationCommandJson(request)
        if (command == null) {
            _investigation.value = badgeInvestigationResult(
                request, "badge", BleInvestigationState.FAILED,
                "Invalid investigation request", "invalid_request",
            )
            return false
        }
        val transport = when (
            BadgeControlTransportPolicy.select(
                hasUsb = hasUsbCommandPath(usbOwnerSnapshot),
                hasBle = hasBleInvestigationPath(),
                hasHttp = activeHttpBaseUrl() != null,
            )
        ) {
            BadgeControlTransport.USB -> BadgeInvestigationTransport.USB
            null -> null
        }
        val operation = synchronized(investigationLock) {
            if (activeInvestigation != null) {
                Log.w(TAG, "Rejecting BLE investigation while another request is active")
                return false
            }
            if (request.route != com.friendorfoe.detection.BleInvestigationRoute.BADGE) {
                _investigation.value = badgeInvestigationResult(
                    request = request,
                    transport = "badge",
                    state = BleInvestigationState.FAILED,
                    summary = "Badge route required",
                    error = "invalid_route",
                )
                return false
            }
            if (request.requestId.length !in 1..32 ||
                request.requestId.any { it.code !in 0x21..0x7E }
            ) {
                _investigation.value = badgeInvestigationResult(
                    request, "badge", BleInvestigationState.FAILED,
                    "Invalid investigation request", "invalid_request_id",
                )
                return false
            }
            if (request.requestId in usedInvestigationRequestIds) {
                _investigation.value = badgeInvestigationResult(
                    request, "badge", BleInvestigationState.FAILED,
                    "Investigation request ID was already used", "request_id_reused",
                )
                return false
            }
            if (transport == null) {
                _investigation.value = badgeInvestigationResult(
                    request, "badge", BleInvestigationState.FAILED,
                    "Badge investigation transport unavailable", "badge_unavailable",
                )
                return false
            }
            if (transport == BadgeInvestigationTransport.USB &&
                (usbOwnerSnapshot == null ||
                    !badgeUsbOwnerKeysMatch(usbOwnerSnapshot, verifiedUsbOwnerKey) ||
                    !hasUsbCommandPath(usbOwnerSnapshot))
            ) {
                _investigation.value = badgeInvestigationResult(
                    request, "badge-usb", BleInvestigationState.FAILED,
                    "Badge USB owner changed before investigation start",
                    "transport_disconnected",
                )
                return false
            }
            rememberInvestigationRequestId(request.requestId)
            investigationGeneration++
            ActiveBadgeInvestigation(
                generation = investigationGeneration,
                request = request,
                command = command,
                transport = transport,
                parser = BadgeInvestigationStreamParser(request.requestId),
                usbOwnerKey = usbOwnerSnapshot.takeIf {
                    transport == BadgeInvestigationTransport.USB
                },
            ).also { activeInvestigation = it }
        }

        val queuedResult = badgeInvestigationResult(
            request = request,
            transport = operation.transport.resultName,
            state = BleInvestigationState.QUEUED,
            summary = "Badge investigation queued",
            error = null,
        )
        val job = scope.launch(start = CoroutineStart.LAZY) {
            try {
                val timeoutMs = badgeInvestigationTotalTimeoutMs(request.timeoutMs)
                val result = withTimeout(timeoutMs) {
                    when (operation.transport) {
                        BadgeInvestigationTransport.USB -> investigateOverUsb(operation)
                        BadgeInvestigationTransport.HTTP -> investigateOverHttp(operation)
                        BadgeInvestigationTransport.BLE -> investigateOverBle(operation)
                    }
                }
                publishInvestigation(operation, result)
            } catch (_: TimeoutCancellationException) {
                publishInvestigation(
                    operation,
                    badgeInvestigationResult(
                        request, operation.transport.resultName, BleInvestigationState.FAILED,
                        "Badge investigation retrieval timed out", "timeout",
                    ),
                )
            } catch (_: CancellationException) {
                // Explicit cancellation publishes before stopping retrieval.
            } catch (error: Exception) {
                Log.w(TAG, "Badge BLE investigation failed", error)
                publishInvestigation(
                    operation,
                    badgeInvestigationResult(
                        request, operation.transport.resultName, BleInvestigationState.FAILED,
                        "Badge investigation failed", "transport_error",
                    ),
                )
            } finally {
                synchronized(investigationLock) {
                    if (activeInvestigation?.generation == operation.generation) {
                        activeInvestigation = null
                    }
                }
                if (operation.transport == BadgeInvestigationTransport.BLE && hasBleCommandPath()) {
                    runCatching { readBleStatus() }
                }
            }
        }
        val shouldStart = synchronized(investigationLock) {
            if (shouldStartBadgeInvestigationJob(
                    activeGeneration = activeInvestigation?.generation,
                    operationGeneration = operation.generation,
                )
            ) {
                operation.job = job
                _investigation.value = queuedResult
                true
            } else {
                false
            }
        }
        if (shouldStart) job.start() else job.cancel()
        return shouldStart
    }

    fun cancelBleInvestigation(requestId: String) {
        val operation = synchronized(investigationLock) {
            val current = activeInvestigation
                ?.takeIf { it.request.requestId == requestId }
                ?: return
            _investigation.value = badgeInvestigationResult(
                request = current.request,
                transport = current.transport.resultName,
                state = BleInvestigationState.CANCELLED,
                summary = "Badge result retrieval cancelled; scanner operation may still be running",
                error = "retrieval_cancelled",
            )
            activeInvestigation = null
            current
        }
        operation.job?.cancel()
    }

    private suspend fun investigateOverUsb(
        operation: ActiveBadgeInvestigation,
    ): BleInvestigationResult {
        val owner = operation.usbOwnerKey ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge USB owner unavailable", "usb_write_failed",
        )
        if (!writeVerifiedUsbLine(
                line = "FOF_CTL:${operation.command}",
                expectedOwner = owner,
            )
        ) {
            return badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED, "Badge USB command failed", "usb_write_failed",
            )
        }
        val ack = try {
            withTimeout(BADGE_CONTROL_ACK_TIMEOUT_MS) { operation.controlAck.await() }
        } catch (_: TimeoutCancellationException) {
            return badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED,
                "Badge did not acknowledge the investigation", "control_ack_timeout",
            )
        }
        if (!ack.accepted) {
            return badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED, "Badge rejected the investigation",
                ack.error ?: "badge_rejected",
            )
        }
        return operation.terminal.await()
    }

    private suspend fun investigateOverHttp(
        operation: ActiveBadgeInvestigation,
    ): BleInvestigationResult {
        val baseUrl = activeHttpBaseUrl() ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge HTTP transport disconnected", "transport_disconnected",
        )
        val startBody = postBadgeInvestigationControl(
            baseUrl,
            operation.command,
        ) ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge HTTP start failed", "http_error",
        )
        val start = parseJsonObject(startBody) ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge HTTP response was malformed", "malformed_response",
        )
        val startOk = start.strictHttpBoolean("ok")
        if (startOk != true) {
            return badgeInvestigationResult(
                operation.request,
                operation.transport.resultName,
                BleInvestigationState.FAILED,
                "Badge rejected the investigation",
                start.strictHttpString("error") ?: "badge_rejected",
            )
        }

        while (true) {
            val statusBody = getBadgeInvestigationStatus(baseUrl)
                ?: return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge HTTP status retrieval failed", "http_status_error",
                )
            val decision = evaluateBadgeHttpInvestigationStatus(
                statusBody,
                operation.request.requestId,
            )
            when (decision.action) {
                BadgeHttpInvestigationAction.FAIL -> return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge returned malformed investigation status",
                    decision.error ?: "malformed_status",
                )
                BadgeHttpInvestigationAction.WAIT -> {
                    decision.state?.takeIf { it != BleInvestigationState.IDLE }?.let { state ->
                        publishInvestigation(
                            operation,
                            badgeInvestigationResult(
                                operation.request,
                                operation.transport.resultName,
                                state,
                                decision.summary.ifBlank { badgeProgressSummary(state) },
                                null,
                            ),
                        )
                    }
                    delay(BADGE_CHUNK_RETRY_MS)
                }
                BadgeHttpInvestigationAction.RETRIEVE ->
                    return retrieveHttpInvestigationChunks(operation, baseUrl, decision)
            }
        }
    }

    private suspend fun retrieveHttpInvestigationChunks(
        operation: ActiveBadgeInvestigation,
        baseUrl: String,
        terminalStatus: BadgeHttpInvestigationDecision,
    ): BleInvestigationResult {
        var seq = 0
        while (seq < BADGE_MAX_INVESTIGATION_CHUNKS) {
            val selection = badgeBleInvestigationChunkCommandJson(
                operation.request.requestId,
                seq,
            ) ?: return badgeHttpTerminalFallback(operation, terminalStatus)
            val body = postBadgeInvestigationControl(baseUrl, selection)
                ?: return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED, "Badge HTTP retrieval failed", "http_error",
                )
            val root = parseJsonObject(body)
                ?: return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge returned a malformed investigation chunk", "malformed_chunk",
                )
            if (root.strictHttpBoolean("ok") == false) {
                return badgeHttpTerminalFallback(operation, terminalStatus)
            }
            val accepted = acceptInvestigationLine(operation, "FOF_INV:$body")
            if (!accepted.accepted) {
                return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge returned an invalid investigation chunk", "invalid_chunk",
                )
            }
            accepted.result?.let { return it.copy(transport = operation.transport.resultName) }
            seq++
        }
        return badgeHttpTerminalFallback(operation, terminalStatus)
    }

    private fun badgeHttpTerminalFallback(
        operation: ActiveBadgeInvestigation,
        terminalStatus: BadgeHttpInvestigationDecision,
    ): BleInvestigationResult {
        finishBadgeHttpInvestigationFromStatus(
            operation.parser,
            operation.request.requestId,
            terminalStatus,
        )?.let { return it.copy(transport = operation.transport.resultName) }

        val state = terminalStatus.state.takeIf {
            it == BleInvestigationState.FAILED || it == BleInvestigationState.CANCELLED
        } ?: BleInvestigationState.FAILED
        val missingTerminal = terminalStatus.state == BleInvestigationState.COMPLETE
        val defaultError = if (state == BleInvestigationState.CANCELLED) "cancelled" else "badge_failed"
        return badgeInvestigationResult(
            operation.request,
            operation.transport.resultName,
            state,
            if (missingTerminal) {
                "Badge investigation ended without a terminal chunk"
            } else {
                terminalStatus.summary.ifBlank { "Badge investigation failed" }
            },
            if (missingTerminal) "missing_terminal" else terminalStatus.error ?: defaultError,
        ).copy(
            authenticationRequired = terminalStatus.authenticationRequired,
            truncated = terminalStatus.truncated,
        )
    }

    private suspend fun investigateOverBle(
        operation: ActiveBadgeInvestigation,
    ): BleInvestigationResult {
        if (!hasBleInvestigationPath()) return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED,
            "Bonded encrypted badge BLE is unavailable", "ble_not_authorized",
        )
        val gatt = activeGatt ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge BLE disconnected", "transport_disconnected",
        )
        val control = activeBleControlChar ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED, "Badge BLE control is unavailable", "ble_unavailable",
        )
        val chunks = activeBleInvestigationChar ?: return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED,
            "Badge BLE investigation data is unavailable", "ble_unavailable",
        )

        val start = writeBleCharacteristic(
            gatt,
            control,
            operation.command.toString().toByteArray(Charsets.UTF_8),
            operation.generation,
        )
        if (isGattAuthenticationError(start.status)) {
            return badgeAuthenticationFailure(operation)
        }
        if (start.status != BluetoothGatt.GATT_SUCCESS) {
            return badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED, "Badge BLE command failed", "gatt_write_failed",
            )
        }

        var seq = 0
        while (seq < BADGE_MAX_INVESTIGATION_CHUNKS) {
            if (!hasBleInvestigationPath() || activeGatt !== gatt) {
                return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge BLE authorization was lost", "ble_not_authorized",
                )
            }
            val selection = badgeBleInvestigationChunkCommandJson(
                operation.request.requestId,
                seq,
            ) ?: return badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED,
                "Badge BLE chunk request was invalid", "invalid_chunk_request",
            )
            val selected = writeBleCharacteristic(
                gatt,
                control,
                selection.toString().toByteArray(Charsets.UTF_8),
                operation.generation,
            )
            if (isGattAuthenticationError(selected.status)) return badgeAuthenticationFailure(operation)
            if (selected.status != BluetoothGatt.GATT_SUCCESS) {
                return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge BLE chunk selection failed", "gatt_write_failed",
                )
            }
            delay(25)
            val read = readBleCharacteristic(gatt, chunks, operation.generation)
            if (isGattAuthenticationError(read.status)) return badgeAuthenticationFailure(operation)
            if (read.status != BluetoothGatt.GATT_SUCCESS) {
                delay(BADGE_CHUNK_RETRY_MS)
                continue
            }
            val json = decodeBadgeUtf8(read.value, read.value.size)
                ?: return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge BLE chunk was not valid UTF-8", "invalid_utf8",
                )
            if (json.toByteArray(Charsets.UTF_8).size > BADGE_INVESTIGATION_JSON_MAX_BYTES ||
                '\n' in json || '\r' in json
            ) {
                return badgeInvestigationResult(
                    operation.request, operation.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge BLE chunk was oversized", "oversized_chunk",
                )
            }
            val accepted = acceptInvestigationLine(operation, "FOF_INV:$json")
            if (!accepted.accepted) {
                delay(BADGE_CHUNK_RETRY_MS)
                continue
            }
            accepted.result?.let { return it.copy(transport = operation.transport.resultName) }
            seq++
        }
        return badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.FAILED,
            "Badge BLE investigation ended without a terminal chunk", "missing_terminal",
        )
    }

    private fun badgeAuthenticationFailure(
        operation: ActiveBadgeInvestigation,
    ): BleInvestigationResult = badgeInvestigationResult(
        operation.request,
        operation.transport.resultName,
        BleInvestigationState.FAILED,
        "Badge BLE authentication is required",
        "authentication_required",
    ).copy(authenticationRequired = true)

    private fun isGattAuthenticationError(status: Int): Boolean =
        status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION || status == 15

    private fun acceptInvestigationLine(
        operation: ActiveBadgeInvestigation,
        line: String,
    ): BadgeInvestigationParseResult {
        val parsed = synchronized(investigationLock) {
            if (activeInvestigation?.generation != operation.generation) {
                return BadgeInvestigationParseResult(accepted = false)
            }
            operation.parser.accept(line)
        }
        operation.ackGate.accept(
            line = line,
            parsed = parsed,
            ownsControlReply = !operation.controlAck.isCompleted,
        )?.let(operation.controlAck::complete)
        if (!parsed.accepted) return parsed
        publishAcceptedChunk(operation, parsed)
        return parsed
    }

    private fun publishAcceptedChunk(
        operation: ActiveBadgeInvestigation,
        parsed: BadgeInvestigationParseResult,
    ) {
        val terminalResult = parsed.result?.copy(transport = operation.transport.resultName)
        if (terminalResult != null) {
            publishInvestigation(operation, terminalResult)
            operation.terminal.complete(terminalResult)
            return
        }
        val current = _investigation.value?.takeIf {
            it.requestId == operation.request.requestId &&
                it.state !in setOf(
                    BleInvestigationState.COMPLETE,
                    BleInvestigationState.FAILED,
                    BleInvestigationState.CANCELLED,
                )
        } ?: badgeInvestigationResult(
            operation.request, operation.transport.resultName,
            BleInvestigationState.QUEUED, "Badge investigation queued", null,
        )
        val updated = when (val chunk = parsed.chunk) {
            is BleInvestigationChunk.Begin -> current.copy(
                state = BleInvestigationState.QUEUED,
                summary = "Badge investigation queued",
            )
            is BleInvestigationChunk.Progress -> current.copy(
                state = chunk.state,
                summary = badgeProgressSummary(chunk.state),
            )
            is BleInvestigationChunk.Service -> current.copy(
                state = BleInvestigationState.READING,
                services = current.services + chunk.uuid,
                summary = "Reading badge investigation result",
            )
            is BleInvestigationChunk.Characteristic -> current.copy(
                state = BleInvestigationState.READING,
                characteristics = current.characteristics +
                    com.friendorfoe.detection.BleGattCharacteristicInfo(
                        serviceUuid = chunk.serviceUuid,
                        uuid = chunk.uuid,
                        properties = chunk.properties,
                    ),
                summary = "Reading badge investigation result",
            )
            is BleInvestigationChunk.Read -> current.copy(
                state = BleInvestigationState.READING,
                reads = current.reads + (chunk.uuid to chunk.valueHex),
                summary = "Reading badge investigation result",
            )
            is BleInvestigationChunk.End,
            null -> current
        }
        publishInvestigation(operation, updated)
    }

    private fun publishInvestigation(
        operation: ActiveBadgeInvestigation,
        result: BleInvestigationResult,
    ) {
        synchronized(investigationLock) {
            if (activeInvestigation?.generation == operation.generation) {
                _investigation.value = result
            }
        }
    }

    private fun terminateInvestigationTransport(
        transport: BadgeInvestigationTransport,
    ) {
        val operation = synchronized(investigationLock) {
            activeInvestigation?.takeIf { it.transport == transport }
        } ?: return
        val result = synchronized(investigationLock) {
            if (activeInvestigation?.generation != operation.generation) null
            else operation.parser.disconnect(operation.request.requestId)
        }?.copy(transport = operation.transport.resultName)
            ?: badgeInvestigationResult(
                operation.request, operation.transport.resultName,
                BleInvestigationState.FAILED,
                "Badge transport disconnected", "transport_disconnected",
            )
        publishInvestigation(operation, result)
        operation.terminal.complete(result)
    }

    private fun detachUsbInvestigationLocked(
        disconnectedOwner: BadgeUsbOwnerKey?,
    ): DetachedBadgeUsbInvestigation? = synchronized(investigationLock) {
            val current = activeInvestigation?.takeIf {
                it.transport == BadgeInvestigationTransport.USB &&
                    it.usbOwnershipGate?.disconnect(disconnectedOwner) == true
            } ?: return@synchronized null
            activeInvestigation = null
            val result = current.parser.disconnect(current.request.requestId)
                ?.copy(transport = current.transport.resultName)
                ?: badgeInvestigationResult(
                    current.request,
                    current.transport.resultName,
                    BleInvestigationState.FAILED,
                    "Badge transport disconnected",
                    "transport_disconnected",
                )
            _investigation.value = result
            DetachedBadgeUsbInvestigation(current, result)
        }

    private fun completeDetachedUsbInvestigation(
        detached: DetachedBadgeUsbInvestigation?,
    ) {
        val operation = detached?.operation ?: return
        operation.controlAck.complete(
            BadgeControlAck(accepted = false, error = "transport_disconnected"),
        )
        operation.terminal.complete(detached.result)
        operation.job?.cancel()
    }

    private fun rememberInvestigationRequestId(requestId: String) {
        usedInvestigationRequestIds += requestId
        while (usedInvestigationRequestIds.size > MAX_USED_INVESTIGATION_IDS) {
            usedInvestigationRequestIds.remove(usedInvestigationRequestIds.first())
        }
    }

    private fun badgeInvestigationResult(
        request: com.friendorfoe.detection.BleInvestigationRequest,
        transport: String,
        state: BleInvestigationState,
        summary: String,
        error: String?,
    ): BleInvestigationResult = BleInvestigationResult(
        requestId = request.requestId,
        transport = transport,
        mode = request.target.mode,
        targetMac = request.target.mac,
        state = state,
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

    private fun badgeProgressSummary(state: BleInvestigationState): String = when (state) {
        BleInvestigationState.QUEUED -> "Badge investigation queued"
        BleInvestigationState.SCANNING -> "Badge scanner is finding the target"
        BleInvestigationState.CONNECTING -> "Badge scanner is connecting"
        BleInvestigationState.DISCOVERING -> "Badge scanner is discovering services"
        BleInvestigationState.READING -> "Badge scanner is reading characteristics"
        else -> "Badge investigation active"
    }

    private suspend fun getBadgeInvestigationStatus(baseUrl: String): String? {
        val request = Request.Builder()
            .url("$baseUrl/api/badge/status")
            .get()
            .build()
        return executeBadgeHttp(request, MAX_BADGE_STATUS_BODY_BYTES)
    }

    private suspend fun postBadgeInvestigationControl(
        baseUrl: String,
        payload: JsonObject,
    ): String? {
        val request = Request.Builder()
            .url("$baseUrl/api/badge/control")
            .post(payload.toString().toRequestBody(jsonMediaType))
            .build()
        return executeBadgeHttp(request, BADGE_INVESTIGATION_JSON_MAX_BYTES)
    }

    private suspend fun executeBadgeHttp(request: Request, maxBytes: Int): String? =
        suspendCancellableCoroutine { continuation ->
            val call = badgeHttpClient.newCall(request)
            continuation.invokeOnCancellation { call.cancel() }
            call.enqueue(object : Callback {
                override fun onFailure(call: Call, e: IOException) {
                    if (continuation.isActive) continuation.resume(null)
                }

                override fun onResponse(call: Call, response: Response) {
                    val body = runCatching {
                        response.use {
                            if (!it.isSuccessful) null
                            else it.body?.readBoundedBytes(maxBytes)
                        }
                    }.getOrNull()
                    if (continuation.isActive) continuation.resume(body)
                }
            })
        }

    private fun ResponseBody.readBoundedBytes(maxBytes: Int): String? {
        val source = source()
        source.request((maxBytes + 1).toLong())
        if (source.buffer.size > maxBytes) return null
        val bytes = source.readByteArray()
        return decodeBadgeUtf8(bytes, bytes.size)
    }

    private fun parseJsonObject(json: String): JsonObject? = runCatching {
        JsonParser.parseString(json).takeIf { it.isJsonObject }?.asJsonObject
    }.getOrNull()

    private fun JsonObject.strictHttpBoolean(key: String): Boolean? {
        val item = get(key) ?: return null
        return item.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isBoolean }?.asBoolean
    }

    private fun JsonObject.strictHttpString(key: String): String? {
        val item = get(key) ?: return null
        return item.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }?.asString
            ?.take(63)
    }

    fun setMode(mode: String) {
        val command = badgeSetModeCommandJson(mode)
        if (command == null) {
            setState { it.copy(message = "Unsupported badge mode") }
            return
        }
        sendControl(command)
    }

    fun rebootBadge() {
        sendControl(badgeRebootCommandJson())
    }

    fun applyDisplayPolicy(policy: BadgeDisplayPolicy, persist: Boolean = true) {
        sendControl(badgeDisplayPolicyCommandJson(policy, persist))
    }

    fun resetDisplayPolicy(persist: Boolean = true) {
        sendControl(badgeDisplayPolicyResetCommandJson(persist))
    }

    fun applyBadgeTheme(theme: BadgeTheme, persist: Boolean = true) {
        sendControl(badgeThemeCommandJson(theme, persist))
    }

    fun resetBadgeTheme(persist: Boolean = true) {
        sendControl(badgeThemeResetCommandJson(persist))
    }

    fun displayNav(action: BadgeDisplayNavAction) {
        sendControl(badgeDisplayNavCommandJson(action))
    }

    private fun sendControl(payload: JsonObject) {
        val command = payload.badgeStrictString("cmd").orEmpty()
        val controlStatus = state.value.controlStatus
        if (!BadgeControlTransportPolicy.allowsAndroidControlCommand(command) ||
            !BadgeControlTransportPolicy.allowsAndroidControlCommand(command, controlStatus)
        ) {
            setState {
                it.copy(
                    message = if (!badgeDisplayControlsAvailable(controlStatus)) {
                        "Display controls are unavailable on this headless badge"
                    } else {
                        "Unsupported Android badge control command"
                    },
                )
            }
            return
        }
        val expectedOwner = verifiedUsbOwnerKey
        scope.launch {
            when (
                BadgeControlTransportPolicy.select(
                    hasUsb = expectedOwner != null && hasUsbCommandPath(expectedOwner),
                    hasBle = hasBleCommandPath(),
                    hasHttp = activeHttpBaseUrl() != null,
                )
            ) {
                BadgeControlTransport.USB -> {
                    if (usbInvestigationOwnsControlReply(expectedOwner)) {
                        setState { it.copy(message = "BLE investigation command is awaiting badge reply") }
                        return@launch
                    }
                    writeVerifiedUsbLine(
                        line = "FOF_CTL:$payload",
                        expectedOwner = expectedOwner ?: return@launch,
                    )
                }
                null -> setState {
                    it.copy(message = BadgeControlTransportPolicy.controlConnectionGuidance())
                }
            }
        }
    }

    private fun startApPoller() {
        if (apPollJob?.isActive == true) return
        apPollJob = scope.launch {
            while (isActive) {
                if (canPollAlternateTransport() && !hasBleCommandPath()) {
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
                if (canPollAlternateTransport() && !hasBleCommandPath() &&
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
                if (canPollAlternateTransport() && !hasBleCommandPath() &&
                    state.value.status != BadgeUsbStatus.AP_CONNECTED
                ) {
                    fetchDebugBridgeStatus(showErrors = false)
                }
                delay(DEBUG_BRIDGE_POLL_INTERVAL_MS)
            }
        }
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun prepareUsbStatusPollerLocked(
        owner: BadgeUsbOwnerKey,
        deviceName: String,
    ): PreparedBadgeUsbStatusPoller {
        val pollJob = scope.launch(start = CoroutineStart.LAZY) {
            val ownJob = coroutineContext[Job] ?: return@launch
            try {
                while (isActive) {
                    val ticket = usbStatusPollGate.beginPoll(owner) ?: break
                    val writeSucceeded = writeVerifiedUsbLine(
                        line = "FOF_STATUS",
                        expectedOwner = owner,
                    )
                    if (!writeSucceeded) break
                    delay(USB_STATUS_POLL_INTERVAL_MS)
                    when (usbStatusPollGate.finishPoll(ticket, owner)) {
                        BadgeUsbStatusPollDecision.FRESH,
                        BadgeUsbStatusPollDecision.MISS -> Unit
                        BadgeUsbStatusPollDecision.TERMINATE -> {
                            connectionMutex.withLock {
                                terminateVerifiedUsbSessionLocked(
                                    expectedOwner = owner,
                                    deviceName = deviceName,
                                    message = "Badge USB status response timed out",
                                )
                            }
                            break
                        }
                        BadgeUsbStatusPollDecision.STALE_OWNER -> break
                    }
                }
            } finally {
                usbStatusPollJobSlot.clear(ownJob)
            }
        }
        val previousJob = usbStatusPollJobSlot.replace(pollJob)
        return PreparedBadgeUsbStatusPoller(pollJob, previousJob)
    }

    private fun startUsbStatusPoller(prepared: PreparedBadgeUsbStatusPoller) {
        prepared.previousJob?.cancel()
        prepared.job.start()
    }

    // Call only while connectionMutex and usbReconnectSelectionGate are held.
    private fun prepareVerifiedUsbReconnectLocked(
        oldOwner: BadgeUsbOwnerKey,
    ): PreparedBadgeUsbReconnect? {
        if (!lifecycleGate.isActive(oldOwner.lifecycleSession)) return null
        val ticket = usbReconnectGate.bind(oldOwner) ?: return null
        val operation = ActiveBadgeUsbReconnectOperation(ticket)
        operation.job = scope.launch(start = CoroutineStart.LAZY) {
            runUsbReconnectScheduler(operation)
        }
        val previous = usbReconnectOperationSlot.replace(operation).also {
            it?.tryTerminalize()
        }
        usbReconnectSelectionGate.advanceStamp()
        return PreparedBadgeUsbReconnect(operation, previous)
    }

    // Call only after the logical failure commit and old transport close complete.
    private fun startVerifiedUsbReconnectLocked(prepared: PreparedBadgeUsbReconnect) {
        prepared.previousOperation?.job?.cancel()
        val operation = prepared.operation
        val mayStart = usbReconnectSelectionGate.withBarrier {
            lifecycleGate.isActive(operation.ticket.lifecycleSession) &&
                usbReconnectOperationSlot.current() === operation &&
                operation.isActive() &&
                usbReconnectGate.isCurrent(
                    operation.ticket,
                    operation.ticket.lifecycleSession,
                )
        }
        if (mayStart) operation.job.start() else cancelUsbReconnect(operation)
    }

    private fun currentUsbReconnectOperation(
        lifecycleSession: Long,
    ): ActiveBadgeUsbReconnectOperation? {
        val operation = usbReconnectOperationSlot.current() ?: return null
        return operation.takeIf {
            usbReconnectGate.isCurrent(it.ticket, lifecycleSession)
        }
    }

    private fun reconnectExpectedHardwareId(
        operation: ActiveBadgeUsbReconnectOperation?,
        lifecycleSession: Long,
    ): String? = operation?.takeIf {
        usbReconnectOperationSlot.current() === it && it.isActive()
    }?.let {
        usbReconnectGate.expectedHardwareId(it.ticket, lifecycleSession)
    }

    private fun reconnectExpectedProductKind(
        operation: ActiveBadgeUsbReconnectOperation?,
        lifecycleSession: Long,
    ): BadgeUsbProductKind? = operation?.takeIf {
        usbReconnectOperationSlot.current() === it && it.isActive()
    }?.let {
        usbReconnectGate.expectedProductKind(it.ticket, lifecycleSession)
    }

    private fun isUsbReconnectAttemptCurrent(
        operation: ActiveBadgeUsbReconnectOperation,
        attempt: ActiveBadgeUsbReconnectAttempt,
        deviceIdentity: BadgeUsbDeviceIdentity,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        expectedConnectionIdentity: Any?,
    ): Boolean = usbReconnectOperationSlot.current() === operation &&
        operation.isActive() &&
        usbReconnectGate.isCurrent(operation.ticket, lifecycleSession) &&
        lifecycleGate.isActive(lifecycleSession) &&
        operation.ticket.lifecycleSession == lifecycleSession &&
        operation.ownsAttempt(attempt) &&
        attempt.ticket === operation.ticket &&
        attempt.lifecycleSession == lifecycleSession &&
        attempt.attachmentToken == attachmentToken &&
        attachmentToken.identity == deviceIdentity &&
        attachmentGate.isCurrent(attachmentToken) &&
        if (expectedConnectionIdentity == null) {
            attempt.connectionIdentity == null
        } else {
            attempt.connectionIdentity === expectedConnectionIdentity
        }

    private fun currentUsbReconnectPermissionAttempt(
        reconnectGeneration: Long,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        deviceIdentity: BadgeUsbDeviceIdentity,
    ): Pair<ActiveBadgeUsbReconnectOperation, ActiveBadgeUsbReconnectAttempt>? {
        val operation = currentUsbReconnectOperation(lifecycleSession) ?: return null
        if (operation.ticket.generation != reconnectGeneration) return null
        val attempt = operation.currentAttempt(lifecycleSession, attachmentToken) ?: return null
        if (!isUsbReconnectAttemptCurrent(
                operation = operation,
                attempt = attempt,
                deviceIdentity = deviceIdentity,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                expectedConnectionIdentity = null,
            )
        ) {
            return null
        }
        return operation to attempt
    }

    private fun currentUsbReconnectOperationForAttempt(
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        connectionIdentity: Any,
    ): ActiveBadgeUsbReconnectOperation? {
        val operation = currentUsbReconnectOperation(lifecycleSession) ?: return null
        val attempt = operation.currentAttempt(lifecycleSession, attachmentToken) ?: return null
        return operation.takeIf {
            isUsbReconnectAttemptCurrent(
                operation = operation,
                attempt = attempt,
                deviceIdentity = attachmentToken.identity,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                expectedConnectionIdentity = connectionIdentity,
            )
        }
    }

    private fun cancelUsbReconnect(
        operation: ActiveBadgeUsbReconnectOperation?,
    ): Boolean {
        if (operation == null) return false
        val cleared = usbReconnectSelectionGate.withBarrier {
            clearUsbReconnectOperationLocked(operation)
        }
        if (cleared) operation.job.cancel()
        return cleared
    }

    private fun clearUsbReconnectOperation(
        operation: ActiveBadgeUsbReconnectOperation,
    ): Boolean = usbReconnectSelectionGate.withBarrier {
        clearUsbReconnectOperationLocked(operation)
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun clearUsbReconnectOperationLocked(
        operation: ActiveBadgeUsbReconnectOperation,
    ): Boolean {
        operation.tryTerminalize()
        if (!usbReconnectOperationSlot.clear(operation)) return false
        clearReconnectUsbPermissionOperationLocked(
            lifecycleSession = operation.ticket.lifecycleSession,
            expectedReconnectOperation = operation,
        )
        usbReconnectGate.clear(operation.ticket)
        operation.clearAttempt()
        usbReconnectSelectionGate.advanceStamp()
        return true
    }

    private fun cancelUsbReconnectForLifecycle(lifecycleSession: Long): Boolean {
        val operation = usbReconnectOperationSlot.current()
            ?.takeIf { it.ticket.lifecycleSession == lifecycleSession }
            ?: return false
        return cancelUsbReconnect(operation)
    }

    private fun cancelUsbReconnectForDetachedOwner(
        operation: ActiveBadgeUsbReconnectOperation,
        reconnectAttemptSnapshot: ActiveBadgeUsbReconnectAttempt?,
        detachedInvalidation: BadgeUsbAttachmentInvalidation?,
        lifecycleSession: Long,
    ) {
        scope.launch {
            connectionMutex.withLock {
                var detachedResources: DetachedBadgeUsbResources? = null
                var cancelOperationJob = false
                usbReconnectSelectionGate.withBarrier {
                    var disconnectedExactAttachment = false
                    val operationStillCurrent =
                        usbReconnectOperationSlot.current() === operation &&
                            usbReconnectGate.isCurrent(
                                operation.ticket,
                                operation.ticket.lifecycleSession,
                            )
                    if (operationStillCurrent) {
                        val reconnectAttempt = operation.currentAttempt()
                            ?.takeIf(operation::ownsAttempt)
                            ?: reconnectAttemptSnapshot?.takeIf(operation::ownsAttempt)
                        if (reconnectAttempt != null) {
                            attachmentGate.invalidateExact(reconnectAttempt.attachmentToken)
                            val connection = reconnectAttempt.connectionIdentity as?
                                android.hardware.usb.UsbDeviceConnection
                            if (connection != null) {
                                detachedResources = revokeUsbSessionLocked(
                                    expectedLifecycleSession = reconnectAttempt.lifecycleSession,
                                    expectedAttachmentToken = reconnectAttempt.attachmentToken,
                                    expectedConnection = connection,
                                )
                                disconnectedExactAttachment = detachedResources != null
                            }
                        }
                        cancelOperationJob = clearUsbReconnectOperationLocked(operation)
                    }
                    val detachedToken = detachedInvalidation?.token
                    if (!disconnectedExactAttachment && detachedToken != null &&
                        activeAttachmentToken == detachedToken
                    ) {
                        detachedResources = revokeUsbSessionLocked(
                            expectedAttachmentToken = detachedToken,
                        )
                        disconnectedExactAttachment = detachedResources != null
                    }
                    if (disconnectedExactAttachment &&
                        lifecycleGate.isActive(lifecycleSession) &&
                        attachmentGate.currentToken() == null
                    ) {
                        setState { current ->
                            reduceBadgeUsbDisconnected(current, "Badge disconnected")
                        }
                    }
                }
                if (cancelOperationJob) operation.job.cancel()
                closeDetachedUsbResources(detachedResources)
            }
            requestConnection()
        }
    }

    private fun cleanupUsbDetachAndRescan(
        invalidation: BadgeUsbAttachmentInvalidation?,
        lifecycleSession: Long,
    ) {
        scope.launch {
            connectionMutex.withLock {
                var detachedResources: DetachedBadgeUsbResources? = null
                usbReconnectSelectionGate.withBarrier {
                    val detachedToken = invalidation?.token
                    if (detachedToken != null && activeAttachmentToken == detachedToken) {
                        detachedResources = revokeUsbSessionLocked(
                            expectedAttachmentToken = detachedToken,
                        )
                        if (detachedResources != null &&
                            lifecycleGate.isActive(lifecycleSession) &&
                            attachmentGate.currentToken() == null
                        ) {
                            setState { current ->
                                reduceBadgeUsbDisconnected(current, "Badge disconnected")
                            }
                        }
                    }
                }
                closeDetachedUsbResources(detachedResources)
            }
            requestConnection()
        }
    }

    private fun failAmbiguousUsbReconnect(
        operation: ActiveBadgeUsbReconnectOperation,
        candidates: List<UsbDevice>,
    ) {
        scope.launch {
            connectionMutex.withLock {
                var detachedResources: DetachedBadgeUsbResources? = null
                var cancelOperationJob = false
                usbReconnectSelectionGate.withBarrier {
                    if (usbReconnectOperationSlot.current() !== operation ||
                        !usbReconnectGate.isCurrent(
                            operation.ticket,
                            operation.ticket.lifecycleSession,
                        ) ||
                        !lifecycleGate.isActive(operation.ticket.lifecycleSession)
                    ) {
                        return@withBarrier
                    }
                    val reconnectAttempt = operation.currentAttempt()
                        ?.takeIf(operation::ownsAttempt)
                    val ownsConnecting = reconnectAttempt?.let { attempt ->
                        badgeUsbReconnectExpiryOwnsConnecting(
                            ticket = operation.ticket,
                            reconnectAttachmentToken = attempt.attachmentToken,
                            reconnectConnectionIdentity = attempt.connectionIdentity,
                            lifecycleActive = lifecycleGate.isActive(
                                operation.ticket.lifecycleSession,
                            ),
                            status = state.value.status,
                            transportLabel = state.value.transportLabel,
                            activeLifecycleSession = activeUsbLifecycleSession,
                            activeAttachmentToken = activeAttachmentToken,
                            activeConnection = activeConnection,
                            activeVerifiedOwner = verifiedUsbOwnerKey,
                        )
                    } == true
                    if (reconnectAttempt != null) {
                        if (attachmentGate.invalidateExact(reconnectAttempt.attachmentToken) != null) {
                            usbReconnectSelectionGate.advanceStamp()
                        }
                        val connection = reconnectAttempt.connectionIdentity as?
                            android.hardware.usb.UsbDeviceConnection
                        if (ownsConnecting && connection != null) {
                            detachedResources = revokeUsbSessionLocked(
                                expectedLifecycleSession = reconnectAttempt.lifecycleSession,
                                expectedAttachmentToken = reconnectAttempt.attachmentToken,
                                expectedConnection = connection,
                            )
                        }
                    }
                    reportAmbiguousBadgeDevices(candidates)
                    cancelOperationJob = clearUsbReconnectOperationLocked(operation)
                }
                if (cancelOperationJob) operation.job.cancel()
                closeDetachedUsbResources(detachedResources)
            }
        }
    }

    private suspend fun runUsbReconnectScheduler(
        operation: ActiveBadgeUsbReconnectOperation,
    ) {
        try {
            while (coroutineContext.isActive) {
                if (usbReconnectOperationSlot.current() !== operation ||
                    !operation.isActive() ||
                    !lifecycleGate.isActive(operation.ticket.lifecycleSession) ||
                    !usbReconnectGate.isCurrent(
                        operation.ticket,
                        operation.ticket.lifecycleSession,
                    )
                ) {
                    break
                }
                when (usbReconnectGate.nextAttempt(
                    operation.ticket,
                    operation.ticket.lifecycleSession,
                )) {
                    BadgeUsbReconnectDecision.RETRY -> {
                        requestConnection(
                            lifecycleSession = operation.ticket.lifecycleSession,
                            preserveRecoveryOnNoCandidates = true,
                            reconnectTicket = operation.ticket,
                        )
                        delay(USB_RECONNECT_INTERVAL_MS)
                    }
                    BadgeUsbReconnectDecision.EXPIRED -> {
                        expireUsbReconnect(operation)
                        break
                    }
                    BadgeUsbReconnectDecision.STALE -> break
                }
            }
        } finally {
            if (operation.isActive()) clearUsbReconnectOperation(operation)
        }
    }

    private suspend fun expireUsbReconnect(
        operation: ActiveBadgeUsbReconnectOperation,
    ) {
        connectionMutex.withLock {
            var detachedResources: DetachedBadgeUsbResources? = null
            usbReconnectSelectionGate.withBarrier {
                if (usbReconnectOperationSlot.current() !== operation ||
                    !operation.isActive() ||
                    !usbReconnectGate.isCurrent(
                        operation.ticket,
                        operation.ticket.lifecycleSession,
                    ) ||
                    !lifecycleGate.isActive(operation.ticket.lifecycleSession)
                ) {
                    return@withBarrier
                }
                if (!operation.tryTerminalize()) return@withBarrier
                if (verifiedUsbOwnerKey != null || state.value.status == BadgeUsbStatus.CONNECTED) {
                    clearUsbReconnectOperationLocked(operation)
                    return@withBarrier
                }
                val reconnectAttempt = operation.currentAttempt()
                    ?.takeIf(operation::ownsAttempt)
                val ownsConnecting = reconnectAttempt?.let { attempt ->
                    badgeUsbReconnectExpiryOwnsConnecting(
                        ticket = operation.ticket,
                        reconnectAttachmentToken = attempt.attachmentToken,
                        reconnectConnectionIdentity = attempt.connectionIdentity,
                        lifecycleActive = lifecycleGate.isActive(
                            operation.ticket.lifecycleSession,
                        ),
                        status = state.value.status,
                        transportLabel = state.value.transportLabel,
                        activeLifecycleSession = activeUsbLifecycleSession,
                        activeAttachmentToken = activeAttachmentToken,
                        activeConnection = activeConnection,
                        activeVerifiedOwner = verifiedUsbOwnerKey,
                    )
                } == true
                if (reconnectAttempt != null) {
                    if (attachmentGate.invalidateExact(reconnectAttempt.attachmentToken) != null) {
                        usbReconnectSelectionGate.advanceStamp()
                    }
                }
                val connection = reconnectAttempt?.connectionIdentity as?
                    android.hardware.usb.UsbDeviceConnection
                if (ownsConnecting && reconnectAttempt != null && connection != null) {
                    detachedResources = revokeUsbSessionLocked(
                        expectedLifecycleSession = reconnectAttempt.lifecycleSession,
                        expectedAttachmentToken = reconnectAttempt.attachmentToken,
                        expectedConnection = connection,
                    )
                }
                setState { current ->
                    reduceBadgeUsbTerminalError(
                        current = current,
                        deviceName = current.deviceName ?: "FoF badge",
                        message = "Badge USB reconnect timed out",
                    )
                }
                clearUsbReconnectOperationLocked(operation)
            }
            closeDetachedUsbResources(detachedResources)
        }
    }

    private fun registerReceiverIfNeeded() {
        receiverLifetimeGate.registerOnce {
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
        }
    }

    private fun findBadgeCandidates(): List<UsbDevice> {
        return usbManager.deviceList.values.filter { device ->
            device.vendorId == ESPRESSIF_VENDOR_ID ||
                device.safeManufacturerName().orEmpty().contains("Espressif", ignoreCase = true) ||
                device.safeProductName().orEmpty().contains("JTAG", ignoreCase = true)
        }
    }

    private fun selectAttachment(
        device: UsbDevice,
        lifecycleSession: Long,
    ): BadgeUsbAttachmentToken {
        val current = attachmentGate.currentToken()
        val forceNewGeneration = current != null &&
            current == activeAttachmentToken &&
            activeUsbLifecycleSession != lifecycleSession
        return attachmentGate.select(
            identity = device.attachmentIdentity(),
            forceNewGeneration = forceNewGeneration,
        )
    }

    private fun captureUsbEnumerationSnapshot(
        lifecycleSession: Long,
    ): BadgeUsbEnumerationSnapshot? = usbReconnectSelectionGate.withBarrier {
        val selectionSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession)
        if (!selectionSnapshot.lifecycleActive) return@withBarrier null
        BadgeUsbEnumerationSnapshot(
            selectionSnapshot = selectionSnapshot,
            // Starting a newer enumeration invalidates every older device-list read,
            // including reads whose eventual selection would be a no-op.
            enumerationEpoch = usbEnumerationEpochGate.advanceEpoch(),
        )
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun usbEnumerationSnapshotIsCurrent(
        snapshot: BadgeUsbEnumerationSnapshot,
    ): Boolean = usbEnumerationEpochGate.isEpochCurrent(snapshot.enumerationEpoch) &&
        usbSelectionSnapshotIsCurrent(snapshot.selectionSnapshot)

    private fun captureUsbSelectionSnapshot(
        lifecycleSession: Long,
    ): BadgeUsbSelectionSnapshot = usbReconnectSelectionGate.withBarrier {
        captureUsbSelectionSnapshotLocked(lifecycleSession)
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun captureUsbSelectionSnapshotLocked(
        lifecycleSession: Long,
    ): BadgeUsbSelectionSnapshot {
        val operation = usbReconnectOperationSlot.current()
        return BadgeUsbSelectionSnapshot(
            stamp = usbReconnectSelectionGate.currentStamp(),
            lifecycleSession = lifecycleSession,
            lifecycleActive = lifecycleGate.isActive(lifecycleSession),
            operation = operation,
            operationGeneration = operation?.ticket?.generation,
            operationActive = operation?.isActive() == true,
            verifiedOwner = verifiedUsbOwnerKey,
            selectedAttachmentToken = attachmentGate.currentToken(),
            activeAttachmentToken = activeAttachmentToken,
            activeConnection = activeConnection,
            activeUsbLifecycleSession = activeUsbLifecycleSession,
            activeUsbIoSession = activeUsbIoSession,
        )
    }

    // Call only while usbReconnectSelectionGate is held.
    private fun usbSelectionSnapshotIsCurrent(
        snapshot: BadgeUsbSelectionSnapshot,
    ): Boolean {
        val operation = usbReconnectOperationSlot.current()
        return usbReconnectSelectionGate.isStampCurrent(snapshot.stamp) &&
            lifecycleGate.isActive(snapshot.lifecycleSession) == snapshot.lifecycleActive &&
            operation === snapshot.operation &&
            operation?.ticket?.generation == snapshot.operationGeneration &&
            (operation?.isActive() == true) == snapshot.operationActive &&
            verifiedUsbOwnerKey === snapshot.verifiedOwner &&
            attachmentGate.currentToken() == snapshot.selectedAttachmentToken &&
            activeAttachmentToken == snapshot.activeAttachmentToken &&
            activeConnection === snapshot.activeConnection &&
            activeUsbLifecycleSession == snapshot.activeUsbLifecycleSession &&
            activeUsbIoSession === snapshot.activeUsbIoSession
    }

    // Call only while usbReconnectSelectionGate is held. A generic enumeration
    // may replace stale work, but it must never mutate a live owner or handshake.
    private fun genericUsbSelectionMayMutate(lifecycleSession: Long): Boolean {
        if (!lifecycleGate.isActive(lifecycleSession) ||
            usbReconnectOperationSlot.current() != null
        ) {
            return false
        }
        val verifiedOwner = verifiedUsbOwnerKey
        if (verifiedOwner != null &&
            lifecycleGate.isActive(verifiedOwner.lifecycleSession)
        ) {
            return false
        }
        val connectionLifecycleSession = activeUsbLifecycleSession
        return (activeConnection == null && activeAttachmentToken == null) ||
            connectionLifecycleSession == null ||
            !lifecycleGate.isActive(connectionLifecycleSession)
    }

    private fun usbPermissionRequestCode(
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        selectionStamp: Long,
        permissionGeneration: Long,
        reconnectGeneration: Long = NO_RECONNECT_GENERATION,
    ): Int {
        var result = lifecycleSession.hashCode()
        result = 31 * result + attachmentToken.generation.hashCode()
        result = 31 * result + attachmentToken.identity.deviceId
        result = 31 * result + attachmentToken.identity.devicePath.hashCode()
        result = 31 * result + reconnectGeneration.hashCode()
        result = 31 * result + selectionStamp.hashCode()
        result = 31 * result + permissionGeneration.hashCode()
        return result
    }

    private fun reportAmbiguousBadgeDevices(candidates: List<UsbDevice>) {
        val names = candidates.take(3).joinToString(", ") { it.displayName() }
        setState {
            it.copy(
                status = BadgeUsbStatus.ERROR,
                deviceName = null,
                message = "Multiple Espressif USB devices found: $names. Attach only the badge over USB-C.",
                transportLabel = "USB-C"
            )
        }
    }

    private suspend fun connectToDevice(
        device: UsbDevice,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        reconnectOperation: ActiveBadgeUsbReconnectOperation? = null,
        reconnectAttempt: ActiveBadgeUsbReconnectAttempt? = null,
        selectionSnapshot: BadgeUsbSelectionSnapshot,
    ) {
        connectionMutex.withLock {
            var exactReconnectAttempt = reconnectAttempt
            var exactSelectionSnapshot = selectionSnapshot
            fun connectContextIsCurrent(expectedConnectionIdentity: Any?): Boolean {
                if (!usbSelectionSnapshotIsCurrent(exactSelectionSnapshot) ||
                    !lifecycleGate.isActive(lifecycleSession) ||
                    device.attachmentIdentity() != attachmentToken.identity ||
                    !attachmentGate.isCurrent(attachmentToken)
                ) {
                    return false
                }
                if (reconnectOperation == null) {
                    return exactReconnectAttempt == null &&
                        currentUsbReconnectOperation(lifecycleSession) == null
                }
                val attempt = exactReconnectAttempt ?: return false
                return isUsbReconnectAttemptCurrent(
                    operation = reconnectOperation,
                    attempt = attempt,
                    deviceIdentity = device.attachmentIdentity(),
                    lifecycleSession = lifecycleSession,
                    attachmentToken = attachmentToken,
                    expectedConnectionIdentity = expectedConnectionIdentity,
                )
            }

            fun commitConnectStateIfCurrent(
                expectedConnectionIdentity: Any?,
                update: (BadgeUsbState) -> BadgeUsbState,
            ): Boolean = usbReconnectSelectionGate.withBarrier {
                if (!connectContextIsCurrent(expectedConnectionIdentity)) {
                    false
                } else {
                    setState(update)
                    true
                }
            }

            val entryConnectionIdentity = exactReconnectAttempt?.connectionIdentity
            var detachedPrevious: DetachedBadgeUsbResources? = null
            val mayOpen = usbReconnectSelectionGate.withBarrier {
                if (!connectContextIsCurrent(entryConnectionIdentity)) return@withBarrier false
                if (entryConnectionIdentity != null) {
                    return@withBarrier false
                }
                if (badgeUsbConnectionCanBeReused(
                        status = state.value.status,
                        activeAttachmentToken = activeAttachmentToken,
                        requestedAttachmentToken = attachmentToken,
                        activeLifecycleSession = activeUsbLifecycleSession,
                        requestedLifecycleSession = lifecycleSession,
                        connectionOpen = activeConnection != null && activeOutEndpoint != null,
                    )
                ) {
                    return@withBarrier false
                }
                detachedPrevious = revokeUsbSessionLocked()
                exactSelectionSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession)
                if (!connectContextIsCurrent(expectedConnectionIdentity = null)) {
                    return@withBarrier false
                }
                setState {
                    it.copy(
                        status = BadgeUsbStatus.CONNECTING,
                        deviceName = device.displayName(),
                        message = "Opening badge USB serial",
                        transportLabel = "USB-C",
                        controlStatus = null
                    )
                }
                true
            }
            closeDetachedUsbResources(detachedPrevious)
            if (!mayOpen) return

            val port = findReadablePort(device)
            if (port == null) {
                commitConnectStateIfCurrent(expectedConnectionIdentity = null) {
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
                commitConnectStateIfCurrent(expectedConnectionIdentity = null) {
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
                closeDetachedUsbResources(
                    DetachedBadgeUsbResources(
                        connection = connection,
                        usbInterface = null,
                        readJob = null,
                        handshakeJob = null,
                        statusPollJob = null,
                        investigation = null,
                        ioDrain = null,
                    ),
                )
                commitConnectStateIfCurrent(expectedConnectionIdentity = null) {
                    it.copy(
                        status = BadgeUsbStatus.ERROR,
                        deviceName = device.displayName(),
                        message = "Could not claim USB badge interface",
                        transportLabel = "USB-C"
                    )
                }
                return
            }

            val connectionBound = usbReconnectSelectionGate.withBarrier {
                if (!connectContextIsCurrent(expectedConnectionIdentity = null)) {
                    return@withBarrier false
                }
                if (reconnectOperation != null) {
                    val attempt = exactReconnectAttempt ?: return@withBarrier false
                    val boundAttempt = reconnectOperation.bindConnection(attempt, connection)
                        ?: return@withBarrier false
                    exactReconnectAttempt = boundAttempt
                    usbReconnectSelectionGate.advanceStamp()
                    exactSelectionSnapshot = captureUsbSelectionSnapshotLocked(lifecycleSession)
                }
                true
            }
            if (!connectionBound) {
                closeDetachedUsbResources(
                    DetachedBadgeUsbResources(
                        connection = connection,
                        usbInterface = port.usbInterface,
                        readJob = null,
                        handshakeJob = null,
                        statusPollJob = null,
                        investigation = null,
                        ioDrain = null,
                    ),
                )
                return
            }
            val ioSession = BadgeUsbIoSession(
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                connection = connection,
                usbInterface = port.usbInterface,
                inEndpoint = port.inEndpoint,
                outEndpoint = port.outEndpoint,
            )
            val publishActiveConnection = {
                publishBadgeUsbIoSession(usbIoArbiter, ioSession) {
                    attachmentGate.activateAndPublishIfCurrent(attachmentToken) {
                        activeUsbIoSession = ioSession
                        activeConnection = connection
                        activeInterface = port.usbInterface
                        activeOutEndpoint = port.outEndpoint
                        activeUsbLifecycleSession = lifecycleSession
                        activeAttachmentToken = attachmentToken
                        setState {
                            it.copy(
                                status = BadgeUsbStatus.CONNECTING,
                                deviceName = device.displayName(),
                                message = "Checking badge USB product identity",
                                transportLabel = "USB-C"
                            )
                        }
                        usbReconnectSelectionGate.advanceStamp()
                    }
                }
            }
            val publicationResult = usbReconnectSelectionGate.withBarrier {
                if (reconnectOperation != null) {
                    var result: BadgeUsbIoPublicationResult? = null
                    reconnectOperation.publishConnectingIfActive {
                        if (connectContextIsCurrent(expectedConnectionIdentity = connection)) {
                            result = publishActiveConnection()
                        }
                    }
                    result ?: BadgeUsbIoPublicationResult(
                        published = false,
                        drain = null,
                    )
                } else if (usbReconnectOperationSlot.current() == null &&
                    connectContextIsCurrent(expectedConnectionIdentity = connection)
                ) {
                    publishActiveConnection()
                } else {
                    BadgeUsbIoPublicationResult(
                        published = false,
                        drain = null,
                    )
                }
            }
            if (!publicationResult.published) {
                usbReconnectSelectionGate.withBarrier {
                    if (reconnectOperation?.clearAttempt(
                            lifecycleSession = lifecycleSession,
                            attachmentToken = attachmentToken,
                            connectionIdentity = connection,
                        ) == true
                    ) {
                        usbReconnectSelectionGate.advanceStamp()
                    }
                }
                closeDetachedUsbResources(
                    DetachedBadgeUsbResources(
                        connection = connection,
                        usbInterface = port.usbInterface,
                        readJob = null,
                        handshakeJob = null,
                        statusPollJob = null,
                        investigation = null,
                        ioDrain = publicationResult.drain,
                    ),
                )
                return
            }
            val preparedReader = prepareUsbReaderLocked(
                connection = connection,
                inEndpoint = port.inEndpoint,
                deviceName = device.displayName(),
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                ioSession = ioSession,
            )
            if (preparedReader == null) return
            if (!startUsbReader(preparedReader)) return
            writeConnectionBoundLineLocked(ioSession, "FOF_PING")
            writeConnectionBoundLineLocked(ioSession, "FOF_STATUS")
            startUsbIdentityHandshake(
                connection = connection,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                deviceName = device.displayName(),
                ioSession = ioSession,
            )
        }
    }

    private fun startUsbIdentityHandshake(
        connection: android.hardware.usb.UsbDeviceConnection,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        deviceName: String,
        ioSession: BadgeUsbIoSession,
    ) {
        val handshakeJob = scope.launch(start = CoroutineStart.LAZY) {
            val ownJob = coroutineContext[Job]
            val deadlineElapsedMs = elapsedRealtimeMs() + USB_IDENTITY_HANDSHAKE_TIMEOUT_MS
            while (isActive) {
                val retryDelayMs = badgeUsbHandshakeDelayMs(
                    nowElapsedMs = elapsedRealtimeMs(),
                    deadlineElapsedMs = deadlineElapsedMs,
                    retryIntervalMs = USB_IDENTITY_HANDSHAKE_RETRY_MS,
                    retryWriteBudgetMs = USB_IDENTITY_HANDSHAKE_WRITE_BUDGET_MS,
                )
                if (retryDelayMs > 0L) delay(retryDelayMs)
                val keepRunning = connectionMutex.withLock {
                    var detachedResources: DetachedBadgeUsbResources? = null
                    val action = usbReconnectSelectionGate.withBarrier {
                        val ownsExactSession = badgeUsbHandshakeOwnsSession(
                            lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                            status = state.value.status,
                            transportLabel = state.value.transportLabel,
                            expectedLifecycleSession = lifecycleSession,
                            activeLifecycleSession = activeUsbLifecycleSession,
                            expectedConnection = connection,
                            activeConnection = activeConnection,
                        ) && activeAttachmentToken == attachmentToken &&
                            attachmentGate.isCurrentAndActive(attachmentToken)
                        val timerAction = badgeUsbHandshakeTimerAction(
                            ownsSession = ownsExactSession,
                            nowElapsedMs = elapsedRealtimeMs(),
                            deadlineElapsedMs = deadlineElapsedMs,
                            retryWriteBudgetMs = USB_IDENTITY_HANDSHAKE_WRITE_BUDGET_MS,
                        )
                        when (timerAction) {
                            BadgeUsbHandshakeTimerAction.STOP -> {
                                if (usbHandshakeJob === ownJob) {
                                    usbHandshakeJob = null
                                    usbReconnectSelectionGate.advanceStamp()
                                }
                            }
                            BadgeUsbHandshakeTimerAction.RETRY -> Unit
                            BadgeUsbHandshakeTimerAction.FAIL -> {
                                detachedResources = revokeUsbSessionLocked(
                                    expectedLifecycleSession = lifecycleSession,
                                    expectedAttachmentToken = attachmentToken,
                                    expectedConnection = connection,
                                )
                                if (detachedResources != null) {
                                    setState {
                                        reduceBadgeUsbTerminalError(
                                            current = it.copy(
                                                status = badgeUsbHandshakeTimeoutStatus(it.status),
                                            ),
                                            deviceName = deviceName,
                                            message = "Badge USB product identity check timed out",
                                        )
                                    }
                                }
                            }
                        }
                        timerAction
                    }
                    if (action == BadgeUsbHandshakeTimerAction.RETRY) {
                        writeConnectionBoundLineLocked(ioSession, "FOF_PING")
                        writeConnectionBoundLineLocked(ioSession, "FOF_STATUS")
                    }
                    closeDetachedUsbResources(detachedResources)
                    action == BadgeUsbHandshakeTimerAction.RETRY
                }
                if (!keepRunning) break
            }
        }
        var previousHandshakeJob: Job? = null
        val installed = usbReconnectSelectionGate.withBarrier {
            val ownsExactSession = badgeUsbHandshakeOwnsSession(
                lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                status = state.value.status,
                transportLabel = state.value.transportLabel,
                expectedLifecycleSession = lifecycleSession,
                activeLifecycleSession = activeUsbLifecycleSession,
                expectedConnection = connection,
                activeConnection = activeConnection,
            ) && activeAttachmentToken == attachmentToken &&
                attachmentGate.isCurrentAndActive(attachmentToken)
            if (!ownsExactSession) return@withBarrier false
            previousHandshakeJob = usbHandshakeJob
            usbHandshakeJob = handshakeJob
            usbReconnectSelectionGate.advanceStamp()
            true
        }
        previousHandshakeJob?.cancel()
        if (installed) handshakeJob.start() else handshakeJob.cancel()
    }

    // Called synchronously from handleLine while withBadgeUsbReaderOwner holds
    // connectionMutex. Keeping rejection in that critical section makes an invalid
    // status fail closed before any later line from the same USB read can be accepted.
    private fun rejectUsbIdentityLocked(
        connection: android.hardware.usb.UsbDeviceConnection,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        expectedVerifiedOwner: BadgeUsbOwnerKey?,
        reconnectOperation: ActiveBadgeUsbReconnectOperation?,
        deviceName: String,
        reason: String,
    ) {
        var detachedResources: DetachedBadgeUsbResources? = null
        var cancelReconnectJob = false
        usbReconnectSelectionGate.withBarrier {
            val ownsExactHandshake = expectedVerifiedOwner == null &&
                badgeUsbHandshakeOwnsSession(
                    lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                    status = state.value.status,
                    transportLabel = state.value.transportLabel,
                    expectedLifecycleSession = lifecycleSession,
                    activeLifecycleSession = activeUsbLifecycleSession,
                    expectedConnection = connection,
                    activeConnection = activeConnection,
                )
            val ownsExactVerifiedSession = expectedVerifiedOwner != null &&
                state.value.status == BadgeUsbStatus.CONNECTED &&
                state.value.transportLabel == "USB-C" &&
                badgeUsbOwnerKeysMatch(expectedVerifiedOwner, verifiedUsbOwnerKey) &&
                expectedVerifiedOwner.connectionIdentity === connection &&
                expectedVerifiedOwner.attachmentToken == attachmentToken
            if ((!ownsExactHandshake && !ownsExactVerifiedSession) ||
                activeAttachmentToken != attachmentToken ||
                !attachmentGate.isCurrentAndActive(attachmentToken)
            ) {
                return@withBarrier
            }
            detachedResources = revokeUsbSessionLocked(
                expectedLifecycleSession = lifecycleSession,
                expectedAttachmentToken = attachmentToken,
                expectedConnection = connection,
            )
            if (detachedResources == null) return@withBarrier
            setState {
                reduceBadgeUsbTerminalError(
                    current = it,
                    deviceName = deviceName,
                    message = reason,
                )
            }
            if (reconnectOperation != null) {
                cancelReconnectJob = clearUsbReconnectOperationLocked(reconnectOperation)
            }
        }
        if (cancelReconnectJob) reconnectOperation?.job?.cancel()
        closeDetachedUsbResources(detachedResources)
    }

    // Call while connectionMutex is held. The reader is created lazy, then its
    // exact transport ownership is published under the selection transaction.
    private fun prepareUsbReaderLocked(
        connection: android.hardware.usb.UsbDeviceConnection,
        inEndpoint: UsbEndpoint,
        deviceName: String,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        ioSession: BadgeUsbIoSession,
    ): PreparedBadgeUsbReader? {
        val readerJob = scope.launch(start = CoroutineStart.LAZY) {
            val ownJob = coroutineContext[Job] ?: return@launch
            val ownsPublishedReader = usbReconnectSelectionGate.withBarrier {
                readJob === ownJob && activeUsbIoSession === ioSession &&
                    badgeUsbReaderOwnsExactSession(
                        lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                        expectedLifecycleSession = lifecycleSession,
                        activeLifecycleSession = activeUsbLifecycleSession,
                        expectedConnection = connection,
                        activeConnection = activeConnection,
                    ) && activeAttachmentToken == attachmentToken &&
                    attachmentGate.isCurrentAndActive(attachmentToken)
            }
            if (!ownsPublishedReader) return@launch
            val buffer = ByteArray(256)
            val readerSilenceGate = BadgeUsbReaderSilenceGate()
            readerSilenceGate.start(elapsedRealtimeMs())
            val lineFramer = BadgeUsbLineFramer(
                onLine = { bytes, length ->
                    decodeBadgeUtf8(bytes, length)?.let { line ->
                        handleLine(
                            line = line,
                            connection = connection,
                            lifecycleSession = lifecycleSession,
                            attachmentToken = attachmentToken,
                            deviceName = deviceName,
                            ioSession = ioSession,
                        )
                    }
                        ?: Log.w(TAG, "Dropping malformed UTF-8 badge line")
                },
                onOverlongLine = {
                    Log.w(TAG, "Dropping overlong badge line")
                },
            )
            try {
                while (isActive) {
                    val lease = usbIoArbiter.tryAcquire(ioSession) ?: break
                    val ioStillCurrent = usbReconnectSelectionGate.withBarrier {
                        activeUsbIoSession === ioSession &&
                            readJob === ownJob &&
                            badgeUsbReaderOwnsExactSession(
                                lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                                expectedLifecycleSession = lifecycleSession,
                                activeLifecycleSession = activeUsbLifecycleSession,
                                expectedConnection = connection,
                                activeConnection = activeConnection,
                            ) && activeAttachmentToken == attachmentToken &&
                            attachmentGate.isCurrentAndActive(attachmentToken) &&
                            ioSession.inEndpoint === inEndpoint
                    }
                    if (!ioStillCurrent) {
                        lease.close()
                        break
                    }
                    val read = try {
                        connection.bulkTransfer(
                            inEndpoint,
                            buffer,
                            buffer.size,
                            READ_TIMEOUT_MS
                        )
                    } finally {
                        lease.close()
                    }
                    val readObservedAtElapsedMs = elapsedRealtimeMs()
                    if (read > 0) {
                        if (!readerSilenceGate.recordRead(read, readObservedAtElapsedMs)) {
                            connectionMutex.withLock {
                                terminateUsbReaderSessionLocked(
                                    connection = connection,
                                    lifecycleSession = lifecycleSession,
                                    attachmentToken = attachmentToken,
                                    ioSession = ioSession,
                                    deviceName = deviceName,
                                    message = "Badge USB reader liveness failed",
                                )
                            }
                            break
                        }
                        connectionMutex.withBadgeUsbReaderOwner(
                            owns = {
                                badgeUsbReaderOwnsExactSession(
                                    lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                                    expectedLifecycleSession = lifecycleSession,
                                    activeLifecycleSession = activeUsbLifecycleSession,
                                    expectedConnection = connection,
                                    activeConnection = activeConnection,
                                ) && activeAttachmentToken == attachmentToken &&
                                    attachmentGate.isCurrentAndActive(attachmentToken)
                            },
                        ) {
                            lineFramer.accept(buffer, read)
                        }
                    } else {
                        if (readerSilenceGate.isExpired(readObservedAtElapsedMs)) {
                            connectionMutex.withLock {
                                terminateUsbReaderSessionLocked(
                                    connection = connection,
                                    lifecycleSession = lifecycleSession,
                                    attachmentToken = attachmentToken,
                                    ioSession = ioSession,
                                    deviceName = deviceName,
                                    message = "Badge USB reader timed out",
                                )
                            }
                            break
                        }
                        delay(25)
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (e: Exception) {
                Log.w(TAG, "Badge USB reader stopped", e)
                connectionMutex.withLock {
                    terminateUsbReaderSessionLocked(
                        connection = connection,
                        lifecycleSession = lifecycleSession,
                        attachmentToken = attachmentToken,
                        ioSession = ioSession,
                        deviceName = deviceName,
                        message = "Badge USB read failed: ${e.message ?: "unknown error"}",
                    )
                }
            }
        }
        var previousJob: Job? = null
        var selectionStamp = NO_SELECTION_STAMP
        val installed = usbReconnectSelectionGate.withBarrier {
            if (!badgeUsbReaderOwnsExactSession(
                    lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                    expectedLifecycleSession = lifecycleSession,
                    activeLifecycleSession = activeUsbLifecycleSession,
                    expectedConnection = connection,
                    activeConnection = activeConnection,
                ) || activeUsbIoSession !== ioSession ||
                state.value.status != BadgeUsbStatus.CONNECTING ||
                state.value.transportLabel != "USB-C" ||
                activeAttachmentToken != attachmentToken ||
                activeOutEndpoint == null ||
                !attachmentGate.isCurrentAndActive(attachmentToken)
            ) {
                return@withBarrier false
            }
            previousJob = readJob
            readJob = readerJob
            selectionStamp = usbReconnectSelectionGate.advanceStamp()
            true
        }
        if (!installed) {
            readerJob.cancel()
            return null
        }
        return PreparedBadgeUsbReader(
            job = readerJob,
            previousJob = previousJob,
            connection = connection,
            lifecycleSession = lifecycleSession,
            attachmentToken = attachmentToken,
            ioSession = ioSession,
            selectionStamp = selectionStamp,
        )
    }

    // Start/cancel only after the selection transaction releases. The coroutine
    // repeats exact ownership validation before its first USB read, closing the
    // stop-between-validation-and-dispatch window without doing I/O in the gate.
    private fun startUsbReader(prepared: PreparedBadgeUsbReader): Boolean {
        prepared.previousJob?.cancel()
        val mayStart = usbReconnectSelectionGate.withBarrier {
            usbReconnectSelectionGate.isStampCurrent(prepared.selectionStamp) &&
                readJob === prepared.job && activeUsbIoSession === prepared.ioSession &&
                badgeUsbReaderOwnsExactSession(
                    lifecycleActive = lifecycleGate.isActive(prepared.lifecycleSession),
                    expectedLifecycleSession = prepared.lifecycleSession,
                    activeLifecycleSession = activeUsbLifecycleSession,
                    expectedConnection = prepared.connection,
                    activeConnection = activeConnection,
                ) && activeAttachmentToken == prepared.attachmentToken &&
                attachmentGate.isCurrentAndActive(prepared.attachmentToken)
        }
        val started = mayStart && prepared.job.start()
        val startStillCurrent = started && usbReconnectSelectionGate.withBarrier {
            usbReconnectSelectionGate.isStampCurrent(prepared.selectionStamp) &&
                readJob === prepared.job && activeUsbIoSession === prepared.ioSession &&
                badgeUsbReaderOwnsExactSession(
                    lifecycleActive = lifecycleGate.isActive(prepared.lifecycleSession),
                    expectedLifecycleSession = prepared.lifecycleSession,
                    activeLifecycleSession = activeUsbLifecycleSession,
                    expectedConnection = prepared.connection,
                    activeConnection = activeConnection,
                ) && activeAttachmentToken == prepared.attachmentToken &&
                attachmentGate.isCurrentAndActive(prepared.attachmentToken)
        }
        if (startStillCurrent) return true
        usbReconnectSelectionGate.withBarrier {
            if (readJob === prepared.job) {
                readJob = null
                usbReconnectSelectionGate.advanceStamp()
            }
        }
        prepared.job.cancel()
        return false
    }

    // Call only while connectionMutex is held. Reader A may finish after reader B
    // replaces it, so the full lifecycle/attachment/connection tuple must still own.
    private fun terminateUsbReaderSessionLocked(
        connection: android.hardware.usb.UsbDeviceConnection,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        ioSession: BadgeUsbIoSession,
        deviceName: String,
        message: String,
    ): Boolean {
        val exactReaderOwner = usbReconnectSelectionGate.withBarrier {
            if (activeUsbIoSession !== ioSession ||
                ioSession.lifecycleSession != lifecycleSession ||
                ioSession.attachmentToken != attachmentToken ||
                ioSession.connection !== connection
            ) {
                return@withBarrier false to null
            }
            val owner = verifiedUsbOwnerKey?.takeIf { owner ->
                owner.lifecycleSession == lifecycleSession &&
                    owner.attachmentToken == attachmentToken &&
                    owner.connectionIdentity === connection &&
                    owner.endpointIdentity === ioSession.outEndpoint
            }
            true to owner
        }
        if (!exactReaderOwner.first) return false
        val expectedVerifiedOwner = exactReaderOwner.second
        if (expectedVerifiedOwner != null) {
            return terminateVerifiedUsbSessionLocked(
                expectedOwner = expectedVerifiedOwner,
                deviceName = deviceName,
                message = message,
            )
        }
        var detachedResources: DetachedBadgeUsbResources? = null
        val committed = usbReconnectSelectionGate.withBarrier {
            if (activeUsbIoSession !== ioSession ||
                !badgeUsbReaderTerminalOwnsExactSession(
                expectedLifecycleSession = lifecycleSession,
                expectedAttachmentToken = attachmentToken,
                expectedConnection = connection,
                lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                status = state.value.status,
                transportLabel = state.value.transportLabel,
                activeLifecycleSession = activeUsbLifecycleSession,
                activeAttachmentToken = activeAttachmentToken,
                attachmentCurrentAndActive =
                    attachmentGate.isCurrentAndActive(attachmentToken),
                activeConnection = activeConnection,
                activeEndpoint = activeOutEndpoint,
                expectedVerifiedOwner = expectedVerifiedOwner,
                activeVerifiedOwner = verifiedUsbOwnerKey,
            )
            ) {
                return@withBarrier false
            }
            detachedResources = revokeUsbSessionLocked(
                expectedLifecycleSession = lifecycleSession,
                expectedAttachmentToken = attachmentToken,
                expectedConnection = connection,
            )
            if (detachedResources == null) return@withBarrier false
            setState { current ->
                reduceBadgeUsbTerminalError(
                    current = current,
                    deviceName = deviceName,
                    message = message,
                )
            }
            true
        }
        closeDetachedUsbResources(detachedResources)
        return committed
    }

    private suspend fun writeConnectionBoundLineLocked(
        ioSession: BadgeUsbIoSession,
        line: String,
    ): Boolean {
        return usbWriteMutex.withLock {
            val lease = usbIoArbiter.tryAcquire(ioSession) ?: return@withLock false
            val connection = ioSession.connection
            val outEndpoint = ioSession.outEndpoint
            val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
            try {
                val ioStillCurrent = usbReconnectSelectionGate.withBarrier {
                    activeUsbIoSession === ioSession &&
                        lifecycleGate.isActive(ioSession.lifecycleSession) &&
                        activeUsbLifecycleSession == ioSession.lifecycleSession &&
                        activeAttachmentToken == ioSession.attachmentToken &&
                        attachmentGate.isCurrentAndActive(ioSession.attachmentToken) &&
                        activeConnection === ioSession.connection &&
                        activeInterface === ioSession.usbInterface &&
                        activeOutEndpoint === ioSession.outEndpoint
                }
                if (!ioStillCurrent) return@withLock false
                withContext(Dispatchers.IO) {
                    connection.bulkTransfer(
                        outEndpoint,
                        bytes,
                        bytes.size,
                        WRITE_TIMEOUT_MS,
                    ) == bytes.size
                }
            } finally {
                lease.close()
            }
        }
    }

    private suspend fun writeVerifiedUsbLine(
        line: String,
        expectedOwner: BadgeUsbOwnerKey,
    ): Boolean = connectionMutex.withLock {
        val verifiedOwner = verifiedUsbOwnerKey ?: return@withLock false
        if (!badgeUsbOwnerKeysMatch(expectedOwner, verifiedOwner)) {
            return@withLock false
        }
        val lifecycleSession = verifiedOwner.lifecycleSession
        val connection = activeConnection
        val outEndpoint = activeOutEndpoint
        val ioSession = activeUsbIoSession
        if (!badgeUsbVerifiedWriteAllowed(
                lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                status = state.value.status,
                transportLabel = state.value.transportLabel,
                activeLifecycleSession = activeUsbLifecycleSession,
                verifiedLifecycleSession = lifecycleSession,
                activeConnection = connection,
                activeEndpoint = outEndpoint,
                verifiedConnection = verifiedOwner.connectionIdentity,
                verifiedEndpoint = verifiedOwner.endpointIdentity,
            ) || connection == null || outEndpoint == null ||
            activeAttachmentToken != verifiedOwner.attachmentToken ||
            !attachmentGate.isCurrentAndActive(verifiedOwner.attachmentToken) ||
            ioSession == null || ioSession.lifecycleSession != lifecycleSession ||
            ioSession.attachmentToken != verifiedOwner.attachmentToken ||
            ioSession.connection !== connection || ioSession.outEndpoint !== outEndpoint
        ) {
            return@withLock false
        }
        val writeSucceeded = writeConnectionBoundLineLocked(
            ioSession = ioSession,
            line = line,
        )
        if (!writeSucceeded) {
            terminateVerifiedUsbSessionLocked(
                expectedOwner = expectedOwner,
                deviceName = state.value.deviceName ?: "FoF badge",
                message = "Badge USB write failed",
            )
        }
        writeSucceeded
    }

    // Call only while connectionMutex is held. A queued failure from owner A must
    // never tear down a replacement owner B that acquired the same USB path.
    private fun terminateVerifiedUsbSessionLocked(
        expectedOwner: BadgeUsbOwnerKey,
        deviceName: String,
        message: String,
    ): Boolean {
        var detachedResources: DetachedBadgeUsbResources? = null
        var preparedReconnect: PreparedBadgeUsbReconnect? = null
        val committed = usbReconnectSelectionGate.withBarrier {
            if (!badgeUsbTerminalFailureOwnsExactSession(
                    expectedOwner = expectedOwner,
                    activeOwner = verifiedUsbOwnerKey,
                    lifecycleActive = lifecycleGate.isActive(expectedOwner.lifecycleSession),
                    status = state.value.status,
                    transportLabel = state.value.transportLabel,
                    activeLifecycleSession = activeUsbLifecycleSession,
                    activeAttachmentToken = activeAttachmentToken,
                    attachmentCurrentAndActive =
                        attachmentGate.isCurrentAndActive(expectedOwner.attachmentToken),
                    activeConnection = activeConnection,
                    activeEndpoint = activeOutEndpoint,
                )
            ) {
                return@withBarrier false
            }
            val connection = expectedOwner.connectionIdentity as?
                android.hardware.usb.UsbDeviceConnection ?: return@withBarrier false
            detachedResources = revokeUsbSessionLocked(
                expectedLifecycleSession = expectedOwner.lifecycleSession,
                expectedAttachmentToken = expectedOwner.attachmentToken,
                expectedConnection = connection,
            )
            if (detachedResources == null) return@withBarrier false
            setState { current ->
                reduceBadgeUsbTerminalError(
                    current = current,
                    deviceName = deviceName,
                    message = message,
                )
            }
            preparedReconnect = prepareVerifiedUsbReconnectLocked(expectedOwner)
            true
        }
        closeDetachedUsbResources(detachedResources)
        preparedReconnect?.let(::startVerifiedUsbReconnectLocked)
        return committed
    }

    private fun hasUsbCommandPath(expectedOwner: BadgeUsbOwnerKey? = null): Boolean {
        val verifiedOwner = verifiedUsbOwnerKey ?: return false
        if (expectedOwner != null && !badgeUsbOwnerKeysMatch(expectedOwner, verifiedOwner)) {
            return false
        }
        val lifecycleSession = verifiedOwner.lifecycleSession
        return badgeUsbVerifiedWriteAllowed(
            lifecycleActive = lifecycleGate.isActive(lifecycleSession),
            status = state.value.status,
            transportLabel = state.value.transportLabel,
            activeLifecycleSession = activeUsbLifecycleSession,
            verifiedLifecycleSession = lifecycleSession,
            activeConnection = activeConnection,
            activeEndpoint = activeOutEndpoint,
            verifiedConnection = verifiedOwner.connectionIdentity,
            verifiedEndpoint = verifiedOwner.endpointIdentity,
        ) && activeAttachmentToken == verifiedOwner.attachmentToken &&
            attachmentGate.isCurrentAndActive(verifiedOwner.attachmentToken)
    }

    private fun hasUsbSession(): Boolean = badgeUsbSessionOwnsTransport(
        status = state.value.status,
        transportLabel = state.value.transportLabel,
        connectionOpen = activeConnection != null && activeOutEndpoint != null,
    )

    private fun canPollAlternateTransport(): Boolean = !badgeUsbStateReservesUsb(
        current = state.value,
        usbConnectionOpen = activeConnection != null && activeOutEndpoint != null,
    )

    private fun hasBleCommandPath(): Boolean {
        if (!BadgeControlTransportPolicy.allowsBleTether()) return false
        return state.value.status == BadgeUsbStatus.BLE_CONNECTED &&
            activeGatt != null &&
            activeBleControlChar != null
    }

    private fun hasBleInvestigationPath(): Boolean {
        val ble = state.value.controlStatus?.bleControl ?: return false
        return hasBleCommandPath() &&
            activeBleInvestigationChar != null &&
            ble.connected &&
            ble.bonded &&
            ble.encrypted
    }

    private fun activeHttpBaseUrl(): String? {
        return when (state.value.status) {
            BadgeUsbStatus.AP_CONNECTED -> BADGE_AP_BASE_URL
            BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED -> DEBUG_BRIDGE_BASE_URL
            else -> null
        }
    }

    private suspend fun fetchNetworkStatus(showErrors: Boolean): Boolean {
        if (fetchApStatus(showErrors = false)) {
            return true
        }
        if (BuildConfig.DEBUG && fetchDebugBridgeStatus(showErrors = false)) {
            return true
        }
        if (showErrors) {
            setState { current ->
                current.copy(message = "Badge AP/Debug Bridge status not reachable; attach the badge over USB-C")
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
                parseBadgeControlStatus(response.body?.string().orEmpty())
            }
        }
        val status = result.getOrNull()
        if (status != null) {
            connectionMutex.withLock {
                setState { current ->
                    reduceBadgeHttpStatus(
                        current = current,
                        response = status,
                        connectedStatus = connectedStatus,
                        deviceName = deviceName,
                        transportLabel = transportLabel,
                        connectedMessage = connectedMessage,
                        usbConnectionOpen = activeConnection != null && activeOutEndpoint != null,
                    )
                }
            }
            true
        } else {
            if (showErrors) {
                connectionMutex.withLock {
                    setState { current ->
                        if (badgeUsbStateReservesUsb(
                                current = current,
                                usbConnectionOpen = activeConnection != null &&
                                    activeOutEndpoint != null,
                            )
                        ) {
                            current
                        } else {
                            current.copy(message = errorMessage)
                        }
                    }
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
        if (!BadgeControlTransportPolicy.allowsBleTether()) return
        if (!hasBlePermissions() || bleScanning || activeGatt != null) {
            if (!hasBlePermissions() && state.value.status == BadgeUsbStatus.DISCONNECTED) {
                setState {
                    it.copy(
                        message = "Attach a FoF badge over USB-C",
                        transportLabel = "USB-C"
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
        if (!BadgeControlTransportPolicy.allowsBleTether()) return
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
            if (!badgeGattDisconnectMatches(activeGatt, gatt)) {
                runCatching { gatt.close() }
                return
            }
            if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                closeBle("Badge BLE disconnected")
                return
            }
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                val mtuStarted = runCatching { gatt.requestMtu(512) }.getOrDefault(false)
                if (!mtuStarted) runCatching { gatt.discoverServices() }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (gatt === activeGatt) runCatching { gatt.discoverServices() }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (gatt !== activeGatt) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                closeBle("Badge BLE service discovery failed")
                return
            }
            val service: BluetoothGattService? = gatt.getService(BADGE_BLE_SERVICE_UUID)
            activeBleStatusChar = service?.getCharacteristic(BADGE_BLE_STATUS_UUID)
            activeBleControlChar = service?.getCharacteristic(BADGE_BLE_CONTROL_UUID)
            activeBleInvestigationChar = service?.getCharacteristic(BADGE_BLE_INVESTIGATION_UUID)
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
            scope.launch {
                enableBleStatusNotifications(gatt, activeBleStatusChar)
                readBleStatus()
            }
        }

        @Suppress("OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: byteArrayOf()
            completeBleGattOperation(
                gatt, BadgeGattOperationKind.READ_CHARACTERISTIC,
                characteristic.uuid, status, value,
            )
            if (status == BluetoothGatt.GATT_SUCCESS && characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                handleBleStatusBytes(value)
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int
        ) {
            completeBleGattOperation(
                gatt, BadgeGattOperationKind.READ_CHARACTERISTIC,
                characteristic.uuid, status, value,
            )
            if (status == BluetoothGatt.GATT_SUCCESS && characteristic.uuid == BADGE_BLE_STATUS_UUID) {
                handleBleStatusBytes(value)
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            completeBleGattOperation(
                gatt, BadgeGattOperationKind.WRITE_CHARACTERISTIC,
                characteristic.uuid, status,
            )
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            completeBleGattOperation(
                gatt, BadgeGattOperationKind.WRITE_DESCRIPTOR,
                descriptor.uuid, status,
            )
        }

        @Suppress("OVERRIDE_DEPRECATION")
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
    private suspend fun enableBleStatusNotifications(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic?
    ) {
        if (characteristic == null || !hasBlePermissions()) return
        if (!runCatching { gatt.setCharacteristicNotification(characteristic, true) }
                .getOrDefault(false)
        ) return
        val descriptor = characteristic.getDescriptor(CLIENT_CONFIG_UUID) ?: return
        writeBleDescriptor(
            gatt,
            descriptor,
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE,
            generation = null,
        )
    }

    @SuppressLint("MissingPermission")
    private suspend fun readBleStatus() {
        val investigationRunning = synchronized(investigationLock) {
            activeInvestigation?.transport == BadgeInvestigationTransport.BLE
        }
        if (investigationRunning) return
        val gatt = activeGatt ?: return
        val characteristic = activeBleStatusChar ?: return
        if (!hasBlePermissions()) return
        readBleCharacteristic(gatt, characteristic, generation = null)
    }

    @SuppressLint("MissingPermission")
    private suspend fun writeBleControl(payload: JsonObject) {
        val gatt = activeGatt
        val characteristic = activeBleControlChar
        if (gatt == null || characteristic == null || !hasBlePermissions()) {
            setState { it.copy(message = "Badge BLE not connected") }
            return
        }
        val bytes = payload.toString().toByteArray(Charsets.UTF_8)
        val result = writeBleCharacteristic(gatt, characteristic, bytes, generation = null)
        val ok = result.status == BluetoothGatt.GATT_SUCCESS
        setState {
            it.copy(message = if (ok) "Badge BLE command sent" else "Badge BLE command failed")
        }
        if (ok) {
            delay(350)
            readBleStatus()
        }
    }

    private suspend fun readBleCharacteristic(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        generation: Long?,
    ): BadgeGattOperationResult = bleGattOperationMutex.withLock {
        if (!isBleGattOperationCurrent(gatt, generation)) return@withLock disconnectedGattResult()
        val pending = PendingBadgeGattOperation(
            gatt = gatt,
            kind = BadgeGattOperationKind.READ_CHARACTERISTIC,
            uuid = characteristic.uuid,
            generation = generation,
        )
        pendingBleGattOperation = pending
        val started = runCatching { gatt.readCharacteristic(characteristic) }.getOrDefault(false)
        if (!started) {
            pendingBleGattOperation = null
            return@withLock BadgeGattOperationResult(BluetoothGatt.GATT_FAILURE)
        }
        awaitBleGattOperation(pending)
    }

    @SuppressLint("MissingPermission")
    private suspend fun writeBleCharacteristic(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        value: ByteArray,
        generation: Long?,
    ): BadgeGattOperationResult = bleGattOperationMutex.withLock {
        if (!isBleGattOperationCurrent(gatt, generation)) return@withLock disconnectedGattResult()
        val pending = PendingBadgeGattOperation(
            gatt = gatt,
            kind = BadgeGattOperationKind.WRITE_CHARACTERISTIC,
            uuid = characteristic.uuid,
            generation = generation,
        )
        pendingBleGattOperation = pending
        val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(
                characteristic,
                value,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            characteristic.value = value
            characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(characteristic)
        }
        if (!started) {
            pendingBleGattOperation = null
            return@withLock BadgeGattOperationResult(BluetoothGatt.GATT_FAILURE)
        }
        awaitBleGattOperation(pending)
    }

    @SuppressLint("MissingPermission")
    private suspend fun writeBleDescriptor(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        value: ByteArray,
        generation: Long?,
    ): BadgeGattOperationResult = bleGattOperationMutex.withLock {
        if (!isBleGattOperationCurrent(gatt, generation)) return@withLock disconnectedGattResult()
        val pending = PendingBadgeGattOperation(
            gatt = gatt,
            kind = BadgeGattOperationKind.WRITE_DESCRIPTOR,
            uuid = descriptor.uuid,
            generation = generation,
        )
        pendingBleGattOperation = pending
        val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(descriptor, value) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            descriptor.value = value
            @Suppress("DEPRECATION")
            gatt.writeDescriptor(descriptor)
        }
        if (!started) {
            pendingBleGattOperation = null
            return@withLock BadgeGattOperationResult(BluetoothGatt.GATT_FAILURE)
        }
        awaitBleGattOperation(pending)
    }

    private suspend fun awaitBleGattOperation(
        pending: PendingBadgeGattOperation,
    ): BadgeGattOperationResult = try {
        withTimeout(2500L) { pending.completion.await() }
    } catch (_: TimeoutCancellationException) {
        if (badgeGattDisconnectMatches(activeGatt, pending.gatt)) {
            closeBle("Badge BLE operation timed out")
        }
        BadgeGattOperationResult(GATT_OPERATION_TIMEOUT)
    } catch (cancelled: CancellationException) {
        if (badgeGattDisconnectMatches(activeGatt, pending.gatt)) {
            closeBle("Badge BLE retrieval cancelled")
        }
        throw cancelled
    } finally {
        if (pendingBleGattOperation === pending) pendingBleGattOperation = null
    }

    private fun completeBleGattOperation(
        gatt: BluetoothGatt,
        kind: BadgeGattOperationKind,
        uuid: UUID,
        status: Int,
        value: ByteArray = byteArrayOf(),
    ) {
        val pending = pendingBleGattOperation ?: return
        val activeGeneration = synchronized(investigationLock) {
            activeInvestigation?.generation
        }
        if (!badgeGattCallbackMatches(
                expectedGatt = pending.gatt,
                actualGatt = gatt,
                expectedKind = pending.kind.name,
                actualKind = kind.name,
                expectedUuid = pending.uuid,
                actualUuid = uuid,
                expectedGeneration = pending.generation,
                activeGeneration = activeGeneration,
            )
        ) return
        pending.completion.complete(BadgeGattOperationResult(status, value.copyOf()))
    }

    private fun isBleGattOperationCurrent(gatt: BluetoothGatt, generation: Long?): Boolean {
        if (activeGatt !== gatt || !hasBlePermissions()) return false
        return generation == null || synchronized(investigationLock) {
            activeInvestigation?.generation == generation &&
                activeInvestigation?.transport == BadgeInvestigationTransport.BLE
        }
    }

    private fun disconnectedGattResult() = BadgeGattOperationResult(GATT_OPERATION_DISCONNECTED)

    private fun handleBleStatusBytes(bytes: ByteArray) {
        val json = decodeBadgeUtf8(bytes, bytes.size)?.trim() ?: return
        if (json.isBlank()) return
        val status = parseBadgeControlStatus(json)
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
        terminateInvestigationTransport(BadgeInvestigationTransport.BLE)
        val gatt = activeGatt
        activeGatt = null
        activeBleControlChar = null
        activeBleStatusChar = null
        activeBleInvestigationChar = null
        pendingBleGattOperation?.completion?.complete(disconnectedGattResult())
        pendingBleGattOperation = null
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

    private fun handleLine(
        line: String,
        connection: android.hardware.usb.UsbDeviceConnection,
        lifecycleSession: Long,
        attachmentToken: BadgeUsbAttachmentToken,
        deviceName: String,
        ioSession: BadgeUsbIoSession,
    ) {
        if (activeUsbIoSession !== ioSession ||
            !badgeUsbReaderOwnsExactSession(
                lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                expectedLifecycleSession = lifecycleSession,
                activeLifecycleSession = activeUsbLifecycleSession,
                expectedConnection = connection,
                activeConnection = activeConnection,
            ) || activeAttachmentToken != attachmentToken ||
            !attachmentGate.isCurrentAndActive(attachmentToken)
        ) {
            return
        }
        val trimmed = line.trim()
        if (trimmed.isEmpty()) return
        val receivedAtElapsedMs = elapsedRealtimeMs()
        val frameOwner = verifiedUsbOwnerKey?.takeIf { owner ->
            state.value.status == BadgeUsbStatus.CONNECTED &&
                owner.attachmentToken == attachmentToken &&
                owner.lifecycleSession == lifecycleSession &&
                owner.connectionIdentity === connection &&
                owner.endpointIdentity === activeOutEndpoint &&
                attachmentGate.isCurrentAndActive(attachmentToken)
        }
        val activeReconnectOperation = currentUsbReconnectOperation(lifecycleSession)
        val reconnectOperation = currentUsbReconnectOperationForAttempt(
            lifecycleSession = lifecycleSession,
            attachmentToken = attachmentToken,
            connectionIdentity = connection,
        )
        if (frameOwner == null && activeReconnectOperation != null &&
            reconnectOperation == null
        ) {
            return
        }
        val expectedHardwareId = reconnectExpectedHardwareId(
            operation = reconnectOperation,
            lifecycleSession = lifecycleSession,
        )
        val detection = if (trimmed.startsWith("FOF_DET:")) {
            parseDetection(trimmed.removePrefix("FOF_DET:"), receivedAtElapsedMs)
        } else {
            null
        }
        val isStatusFrame = trimmed.startsWith("FOF_STATUS:")
        val status = if (isStatusFrame) {
            parseBadgeControlStatus(
                trimmed.removePrefix("FOF_STATUS:"),
                snapshotAtElapsedMs = receivedAtElapsedMs,
            )
        } else {
            null
        }
        val liteLiveReady = if (frameOwner?.productKind == BadgeUsbProductKind.BADGE_LITE) {
            parseBadgeUsbLiteLiveReady(trimmed)
        } else {
            null
        }
        val liteLiveHeartbeat = if (frameOwner?.productKind == BadgeUsbProductKind.BADGE_LITE) {
            parseBadgeUsbLiteLiveHeartbeat(trimmed)
        } else {
            null
        }
        var liteLiveFrameAccepted = false
        val usbHandshakeActive = badgeUsbHandshakeOwnsSession(
            lifecycleActive = lifecycleGate.isActive(lifecycleSession),
            status = state.value.status,
            transportLabel = state.value.transportLabel,
            expectedLifecycleSession = lifecycleSession,
            activeLifecycleSession = activeUsbLifecycleSession,
            expectedConnection = connection,
            activeConnection = activeConnection,
        ) && activeAttachmentToken == attachmentToken &&
            attachmentGate.isCurrentAndActive(attachmentToken)
        val handshakeStatus = if (usbHandshakeActive && isStatusFrame) {
            badgeUsbHandshakeStatus(status)
        } else {
            null
        }
        badgeUsbStatusFrameIdentityError(
            isStatusFrame = isStatusFrame,
            status = status,
            expectedHardwareId = frameOwner?.hardwareId ?: expectedHardwareId,
            expectedProductKind = frameOwner?.productKind ?: reconnectExpectedProductKind(
                operation = reconnectOperation,
                lifecycleSession = lifecycleSession,
            ),
        )?.let { identityError ->
            rejectUsbIdentityLocked(
                connection = connection,
                lifecycleSession = lifecycleSession,
                attachmentToken = attachmentToken,
                expectedVerifiedOwner = frameOwner,
                reconnectOperation = reconnectOperation,
                deviceName = deviceName,
                reason = "USB device does not report FoF badge uplink identity: " +
                    identityError,
            )
            return
        }
        val usbHandshakeAccepted = handshakeStatus == BadgeUsbStatus.CONNECTED
        val acceptedOwner = if (usbHandshakeAccepted) {
            val outEndpoint = activeOutEndpoint ?: return
            badgeUsbOwnerKeyFromHandshake(
                status = status,
                attachmentToken = attachmentToken,
                lifecycleSession = lifecycleSession,
                connectionIdentity = connection,
                endpointIdentity = outEndpoint,
            ) ?: return
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
        fun frameStateUpdate(
            investigationHandled: Boolean,
        ): (BadgeUsbState) -> BadgeUsbState {
            val activity = badgeUsbActivityForLine(
                line = trimmed,
                receivedAtElapsedMs = receivedAtElapsedMs,
                detection = detection,
                status = status,
                firmwareProgress = firmwareProgress,
                investigationHandled = investigationHandled,
            )
            return { current ->
                val nextDetections = detection?.let {
                    (listOf(it) + current.detections).take(MAX_RECENT_DETECTIONS)
                } ?: current.detections

                current.copy(
                    status = handshakeStatus ?: current.status,
                    lastLine = trimmed.take(160),
                    eventCount = if (detection != null) current.eventCount + 1 else current.eventCount,
                    detections = nextDetections,
                    activity = activity?.let { pushBadgeUsbActivity(current.activity, it) }
                        ?: current.activity,
                    controlStatus = status ?: current.controlStatus,
                    firmwareProgress = firmwareProgress ?: current.firmwareProgress,
                    message = when {
                        usbHandshakeAccepted -> "Badge USB connected"
                        trimmed.startsWith("FOF_PONG:") ->
                            "Badge replied ${trimmed.removePrefix("FOF_PONG:")}"
                        status != null -> "Badge status updated"
                        firmwareProgress != null -> firmwareProgress.error.ifBlank {
                            "Firmware ${firmwareProgress.kind} ${firmwareProgress.stage} " +
                                "${firmwareProgress.percent}%"
                        }
                        trimmed.startsWith("FOF_CTL_OK:") -> "Badge command accepted"
                        trimmed.startsWith("FOF_CTL_ERROR:") -> "Badge command failed"
                        liteLiveFrameAccepted && liteLiveReady != null ->
                            "Badge Lite live stream ready"
                        liteLiveFrameAccepted && liteLiveHeartbeat != null ->
                            "Badge Lite live stream active"
                        investigationHandled -> "Badge investigation updated"
                        detection != null -> "Receiving badge events"
                        else -> current.message
                    }
                )
            }
        }
        if (acceptedOwner != null) {
            var preparedStatusPoller: PreparedBadgeUsbStatusPoller? = null
            var completedReconnectJob: Job? = null
            var completedHandshakeJob: Job? = null
            val publishVerifiedOwner = {
                var published = false
                attachmentGate.publishIfCurrentAndActive(attachmentToken) {
                    if (activeUsbIoSession === ioSession &&
                        badgeUsbReaderOwnsExactSession(
                            lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                            expectedLifecycleSession = lifecycleSession,
                            activeLifecycleSession = activeUsbLifecycleSession,
                            expectedConnection = connection,
                            activeConnection = activeConnection,
                        ) && activeAttachmentToken == attachmentToken &&
                        activeOutEndpoint === acceptedOwner.endpointIdentity
                    ) {
                        usbStatusPollGate.bind(
                            owner = acceptedOwner,
                            initialResponsesCompleted = badgeUsbStatusResponseCounter(status),
                        )
                        if (acceptedOwner.productKind == BadgeUsbProductKind.BADGE_LITE) {
                            check(usbLiteLiveGate.bind(acceptedOwner)) {
                                "Verified Badge Lite owner could not bind live USB session"
                            }
                        }
                        verifiedUsbOwnerKey = acceptedOwner
                        setState(frameStateUpdate(false))
                        preparedStatusPoller = prepareUsbStatusPollerLocked(
                            acceptedOwner,
                            deviceName,
                        )
                        completedHandshakeJob = usbHandshakeJob
                        usbHandshakeJob = null
                        usbReconnectSelectionGate.advanceStamp()
                        published = true
                    }
                }
                published
            }
            val verifiedOwnerPublished = usbReconnectSelectionGate.withBarrier {
                if (reconnectOperation != null) {
                    reconnectOperation.completeHandshakeAndClearIfActive(
                        operationIsCurrent = {
                            usbReconnectOperationSlot.current() === reconnectOperation &&
                                usbReconnectGate.isCurrent(
                                    reconnectOperation.ticket,
                                    reconnectOperation.ticket.lifecycleSession,
                                ) && lifecycleGate.isActive(
                                    reconnectOperation.ticket.lifecycleSession,
                                )
                        },
                        fullCommit = publishVerifiedOwner,
                        completion = {
                            if (usbReconnectOperationSlot.clear(reconnectOperation)) {
                                usbReconnectGate.clear(reconnectOperation.ticket)
                                reconnectOperation.clearAttempt()
                                completedReconnectJob = reconnectOperation.job
                            }
                        },
                    )
                } else if (usbReconnectOperationSlot.current() == null) {
                    publishVerifiedOwner()
                } else {
                    false
                }
            }
            if (!verifiedOwnerPublished) return
            completedReconnectJob?.cancel()
            completedHandshakeJob?.cancel()
            preparedStatusPoller?.let(::startUsbStatusPoller)
            badgeUsbLiteLiveStartLine(acceptedOwner)?.let { liveStartLine ->
                scope.launch {
                    writeVerifiedUsbLine(
                        line = liveStartLine,
                        expectedOwner = acceptedOwner,
                    )
                }
            }
            return
        }
        if (!badgeUsbFrameMayMutateState(
                hasVerifiedOwner = frameOwner != null,
                acceptedIdentityHandshake = usbHandshakeAccepted,
            )
        ) {
            return
        }
        val exactFrameOwner = frameOwner ?: return
        var preparedInvestigation = PreparedBadgeUsbInvestigationFrame()
        var liteLiveAckTicket: BadgeUsbLiteLiveAckTicket? = null
        val frameCommitted = usbReconnectSelectionGate.withBarrier {
            val currentOwner = verifiedUsbOwnerKey
            if (activeUsbIoSession !== ioSession ||
                !lifecycleGate.isActive(lifecycleSession) ||
                activeUsbLifecycleSession != lifecycleSession ||
                activeAttachmentToken != attachmentToken ||
                activeConnection !== connection ||
                !attachmentGate.isCurrentAndActive(attachmentToken) ||
                !badgeUsbOwnerKeysMatch(exactFrameOwner, currentOwner)
            ) {
                return@withBarrier false
            }
            preparedInvestigation = prepareUsbInvestigationFrameLocked(
                line = if (line.startsWith("FOF_INV:")) line else trimmed,
                frameOwner = exactFrameOwner,
            )
            if (isStatusFrame) {
                usbStatusPollGate.recordStatus(
                    owner = exactFrameOwner,
                    responsesCompleted = badgeUsbStatusResponseCounter(status),
                )
            }
            if (liteLiveReady != null) {
                liteLiveFrameAccepted = usbLiteLiveGate.acceptReady(
                    owner = exactFrameOwner,
                    ready = liteLiveReady,
                )
            } else if (liteLiveHeartbeat != null) {
                liteLiveAckTicket = usbLiteLiveGate.prepareAck(
                    owner = exactFrameOwner,
                    heartbeat = liteLiveHeartbeat,
                )
                liteLiveFrameAccepted = liteLiveAckTicket != null
            }
            val frameStateUpdate = frameStateUpdate(preparedInvestigation.handled)
            setState(frameStateUpdate)
            true
        }
        if (!frameCommitted) return
        completePreparedUsbInvestigationFrame(preparedInvestigation)
        liteLiveAckTicket?.let { ticket ->
            scope.launch {
                val ackLine = badgeUsbLiteLiveAckLine(ticket.sessionId, ticket.sequence)
                val sent = ackLine != null && writeVerifiedUsbLine(
                    line = ackLine,
                    expectedOwner = ticket.owner,
                )
                usbLiteLiveGate.completeAck(ticket, sent)
            }
        }
    }

    // Call only while connectionMutex -> usbReconnectSelectionGate are held.
    // Parser/state ownership is consumed atomically; Deferred completions happen
    // after the selection transaction releases.
    private fun prepareUsbInvestigationFrameLocked(
        line: String,
        frameOwner: BadgeUsbOwnerKey,
    ): PreparedBadgeUsbInvestigationFrame = synchronized(investigationLock) {
        val operation = activeInvestigation?.takeIf {
                it.transport == BadgeInvestigationTransport.USB &&
                    it.usbOwnershipGate?.acceptsFrame(frameOwner, state.value.status) == true
            } ?: return@synchronized PreparedBadgeUsbInvestigationFrame()
        val parsed = if (line.startsWith("FOF_INV:")) {
            operation.parser.accept(line)
        } else {
            null
        }
        val ack = operation.ackGate.accept(
            line = line,
            parsed = parsed,
            ownsControlReply = !operation.controlAck.isCompleted,
        )
        var terminalResult: BleInvestigationResult? = null
        if (parsed?.accepted == true) {
            terminalResult = parsed.result?.copy(transport = operation.transport.resultName)
            if (terminalResult != null) {
                _investigation.value = terminalResult
            } else {
                val current = _investigation.value?.takeIf {
                    it.requestId == operation.request.requestId &&
                        it.state !in setOf(
                            BleInvestigationState.COMPLETE,
                            BleInvestigationState.FAILED,
                            BleInvestigationState.CANCELLED,
                        )
                } ?: badgeInvestigationResult(
                    operation.request,
                    operation.transport.resultName,
                    BleInvestigationState.QUEUED,
                    "Badge investigation queued",
                    null,
                )
                _investigation.value = when (val chunk = parsed.chunk) {
                    is BleInvestigationChunk.Begin -> current.copy(
                        state = BleInvestigationState.QUEUED,
                        summary = "Badge investigation queued",
                    )
                    is BleInvestigationChunk.Progress -> current.copy(
                        state = chunk.state,
                        summary = badgeProgressSummary(chunk.state),
                    )
                    is BleInvestigationChunk.Service -> current.copy(
                        state = BleInvestigationState.READING,
                        services = current.services + chunk.uuid,
                        summary = "Reading badge investigation result",
                    )
                    is BleInvestigationChunk.Characteristic -> current.copy(
                        state = BleInvestigationState.READING,
                        characteristics = current.characteristics +
                            com.friendorfoe.detection.BleGattCharacteristicInfo(
                                serviceUuid = chunk.serviceUuid,
                                uuid = chunk.uuid,
                                properties = chunk.properties,
                            ),
                        summary = "Reading badge investigation result",
                    )
                    is BleInvestigationChunk.Read -> current.copy(
                        state = BleInvestigationState.READING,
                        reads = current.reads + (chunk.uuid to chunk.valueHex),
                        summary = "Reading badge investigation result",
                    )
                    is BleInvestigationChunk.End,
                    null -> current
                }
            }
        }
        PreparedBadgeUsbInvestigationFrame(
            handled = parsed?.accepted == true || ack != null,
            controlAck = ack?.let { operation.controlAck to it },
            terminal = terminalResult?.let { operation.terminal to it },
        )
    }

    private fun completePreparedUsbInvestigationFrame(
        prepared: PreparedBadgeUsbInvestigationFrame,
    ) {
        prepared.controlAck?.let { (completion, value) -> completion.complete(value) }
        prepared.terminal?.let { (completion, value) -> completion.complete(value) }
    }

    private fun usbInvestigationOwnsControlReply(
        expectedOwner: BadgeUsbOwnerKey?,
    ): Boolean {
        return synchronized(investigationLock) {
            activeInvestigation?.let {
                it.transport == BadgeInvestigationTransport.USB &&
                    badgeUsbOwnerKeysMatch(it.usbOwnerKey, expectedOwner) &&
                    !it.controlAck.isCompleted
            } == true
        }
    }

    private fun parseDetection(
        json: String,
        receivedAtElapsedMs: Long,
    ): BadgeUsbDetection? =
        parseBadgeUsbDetection(json, receivedAtElapsedMs)

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

    private fun disconnect(
        reason: String,
        lifecycleSession: Long? = null,
        expectedAttachmentToken: BadgeUsbAttachmentToken? = null,
        expectedConnection: android.hardware.usb.UsbDeviceConnection? = null,
        expectedSnapshot: BadgeUsbSelectionSnapshot? = null,
    ) {
        scope.launch {
            connectionMutex.withLock {
                var detachedResources: DetachedBadgeUsbResources? = null
                usbReconnectSelectionGate.withBarrier {
                    val ownsExpectedConnection = when {
                        expectedSnapshot != null ->
                            activeUsbLifecycleSession == expectedSnapshot.activeUsbLifecycleSession &&
                                activeAttachmentToken == expectedSnapshot.activeAttachmentToken &&
                                activeConnection === expectedSnapshot.activeConnection
                        expectedAttachmentToken != null && expectedConnection != null ->
                            badgeUsbCleanupOwnsActive(
                                expectedAttachmentToken = expectedAttachmentToken,
                                expectedConnection = expectedConnection,
                                activeAttachmentToken = activeAttachmentToken,
                                activeConnection = activeConnection,
                            )
                        lifecycleSession != null -> activeUsbLifecycleSession == lifecycleSession
                        expectedAttachmentToken != null ->
                            activeAttachmentToken == expectedAttachmentToken
                        else -> activeAttachmentToken == null && activeConnection == null
                    }
                    if (!ownsExpectedConnection) return@withBarrier
                    detachedResources = revokeUsbSessionLocked(
                        expectedLifecycleSession = expectedSnapshot
                            ?.activeUsbLifecycleSession ?: lifecycleSession,
                        expectedAttachmentToken = expectedSnapshot
                            ?.activeAttachmentToken ?: expectedAttachmentToken,
                        expectedConnection = (expectedSnapshot?.activeConnection as?
                            android.hardware.usb.UsbDeviceConnection) ?: expectedConnection,
                        expectedSnapshot = expectedSnapshot,
                    )
                    if (lifecycleGate.activeSession() == null &&
                        activeConnection == null &&
                        attachmentGate.currentToken() == null
                    ) {
                        setState { current -> reduceBadgeUsbDisconnected(current, reason) }
                    }
                }
                closeDetachedUsbResources(detachedResources)
            }
        }
    }

    // Call only while connectionMutex and usbReconnectSelectionGate are held.
    // This method is deliberately logical-only; callbacks and platform I/O run
    // from closeDetachedUsbResources after the selection transaction releases.
    private fun revokeUsbSessionLocked(
        expectedLifecycleSession: Long? = null,
        expectedAttachmentToken: BadgeUsbAttachmentToken? = null,
        expectedConnection: android.hardware.usb.UsbDeviceConnection? = null,
        expectedSnapshot: BadgeUsbSelectionSnapshot? = null,
    ): DetachedBadgeUsbResources? {
        if (expectedSnapshot != null &&
            (activeUsbLifecycleSession != expectedSnapshot.activeUsbLifecycleSession ||
                activeAttachmentToken != expectedSnapshot.activeAttachmentToken ||
                activeConnection !== expectedSnapshot.activeConnection ||
                activeUsbIoSession !== expectedSnapshot.activeUsbIoSession)
        ) {
            return null
        }
        if (expectedLifecycleSession != null &&
            activeUsbLifecycleSession != expectedLifecycleSession
        ) {
            return null
        }
        if (expectedAttachmentToken != null &&
            activeAttachmentToken != expectedAttachmentToken
        ) {
            return null
        }
        if (expectedConnection != null && activeConnection !== expectedConnection) {
            return null
        }

        val disconnectedAttachmentToken = activeAttachmentToken
        val disconnectedLifecycleSession = activeUsbLifecycleSession
        val connection = activeConnection
        val usbInterface = activeInterface
        val disconnectedOwner = verifiedUsbOwnerKey
        val ioSession = activeUsbIoSession
        val ioDrain = ioSession?.let { usbIoArbiter.revoke(ioSession) }
        val detachedReadJob = readJob
        val detachedHandshakeJob = usbHandshakeJob
        val detachedStatusPollJob = usbStatusPollJobSlot.take()
        if (connection == null && usbInterface == null && disconnectedOwner == null &&
            disconnectedAttachmentToken == null && detachedReadJob == null &&
            detachedHandshakeJob == null && detachedStatusPollJob == null &&
            ioSession == null
        ) {
            return null
        }

        if (disconnectedAttachmentToken != null &&
            disconnectedLifecycleSession != null &&
            connection != null
        ) {
            currentUsbReconnectOperation(disconnectedLifecycleSession)?.clearAttempt(
                lifecycleSession = disconnectedLifecycleSession,
                attachmentToken = disconnectedAttachmentToken,
                connectionIdentity = connection,
            )
        }
        verifiedUsbOwnerKey = null
        if (disconnectedOwner != null) {
            usbStatusPollGate.clear(disconnectedOwner)
            usbLiteLiveGate.clear(disconnectedOwner)
        }
        val detachedInvestigation = detachUsbInvestigationLocked(disconnectedOwner)
        readJob = null
        usbHandshakeJob = null
        activeConnection = null
        activeInterface = null
        activeOutEndpoint = null
        activeUsbLifecycleSession = null
        activeAttachmentToken = null
        activeUsbIoSession = null
        if (disconnectedAttachmentToken != null) {
            attachmentGate.deactivateExact(disconnectedAttachmentToken)
        }
        usbReconnectSelectionGate.advanceStamp()
        return DetachedBadgeUsbResources(
            connection = connection,
            usbInterface = usbInterface,
            readJob = detachedReadJob,
            handshakeJob = detachedHandshakeJob,
            statusPollJob = detachedStatusPollJob,
            investigation = detachedInvestigation,
            ioDrain = ioDrain,
        )
    }

    private fun closeDetachedUsbResources(detached: DetachedBadgeUsbResources?) {
        if (detached == null) return
        detached.readJob?.cancel()
        detached.handshakeJob?.cancel()
        detached.statusPollJob?.cancel()
        completeDetachedUsbInvestigation(detached.investigation)
        val ioDrain = detached.ioDrain
        val cleanup = RetainedBadgeUsbDrainCleanup(detached)
        if (ioDrain == null) {
            check(cleanup.phaseGate.markDrained()) {
                "Fresh close-only badge USB cleanup did not enter drained phase"
            }
            if (!advanceUsbIoCleanup(cleanup)) {
                scheduleLateUsbDrain(cleanup)
            }
            return
        }

        if (!usbIoArbiter.awaitDrained(ioDrain, USB_IO_DRAIN_TIMEOUT_MS)) {
            Log.w(TAG, "Badge USB I/O is still draining; retaining handle for late close")
            scheduleLateUsbDrain(cleanup)
            return
        }
        check(cleanup.phaseGate.markDrained()) {
            "Fresh badge USB cleanup did not enter drained phase"
        }
        if (!advanceUsbIoCleanup(cleanup)) {
            scheduleLateUsbDrain(cleanup)
        }
    }

    private fun advanceUsbIoCleanup(cleanup: RetainedBadgeUsbDrainCleanup): Boolean {
        val ioDrain = cleanup.detached.ioDrain
        if (cleanup.phaseGate.shouldAttemptClose()) {
            if (!physicallyCloseDetachedUsbResources(cleanup.detached)) return false
            check(cleanup.phaseGate.markClosed()) {
                "Badge USB cleanup close phase changed unexpectedly"
            }
        }
        if (cleanup.phaseGate.shouldAttemptDrainCompletion()) {
            if (ioDrain != null && !usbIoArbiter.completeDrain(ioDrain)) return false
            check(cleanup.phaseGate.markCompleted()) {
                "Badge USB cleanup completion phase changed unexpectedly"
            }
        }
        return cleanup.phaseGate.isCompleted()
    }

    private fun physicallyCloseDetachedUsbResources(
        detached: DetachedBadgeUsbResources,
    ): Boolean {
        val connection = detached.connection
        val usbInterface = detached.usbInterface
        if (connection != null && usbInterface != null) {
            runCatching { connection.releaseInterface(usbInterface) }
                .onFailure { Log.w(TAG, "Badge USB interface release failed", it) }
        }
        return runCatching { connection?.close() }
            .onFailure { Log.w(TAG, "Badge USB connection close failed", it) }
            .isSuccess
    }

    private fun scheduleLateUsbDrain(cleanup: RetainedBadgeUsbDrainCleanup) {
        val ioDrain = cleanup.detached.ioDrain
        lateinit var reaperJob: Job
        reaperJob = scope.launch(start = CoroutineStart.LAZY) {
            var releasedUnderConnectionMutex = false
            try {
                while (isActive && !cleanup.phaseGate.isCompleted()) {
                    if (cleanup.phaseGate.phaseName() == "DRAINING") {
                        val drained = ioDrain == null || usbIoArbiter.awaitDrained(
                            ioDrain,
                            USB_IO_DRAIN_TIMEOUT_MS,
                        )
                        if (drained) {
                            check(cleanup.phaseGate.markDrained()) {
                                "Badge USB cleanup drain phase changed unexpectedly"
                            }
                        }
                    } else {
                        connectionMutex.withLock {
                            if (advanceUsbIoCleanup(cleanup) &&
                                cleanup.phaseGate.isCompleted()
                            ) {
                                check(lateUsbCleanupSlot.finishWorker(
                                    cleanup = cleanup,
                                    worker = reaperJob,
                                    completed = true,
                                )) {
                                    "Completed badge USB cleanup lost its retained worker"
                                }
                                releasedUnderConnectionMutex = true
                            }
                        }
                    }
                    if (!cleanup.phaseGate.isCompleted()) {
                        delay(USB_IO_CLEANUP_RETRY_MS)
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                Log.e(TAG, "Badge USB late cleanup worker failed", failure)
            } finally {
                val completed = cleanup.phaseGate.isCompleted()
                val releasedWorker = if (!releasedUnderConnectionMutex) {
                    lateUsbCleanupSlot.finishWorker(
                        cleanup = cleanup,
                        worker = reaperJob,
                        completed = completed,
                    )
                } else {
                    true
                }
                val repositoryActive = scope.coroutineContext[Job]?.isActive == true
                if (releasedWorker && repositoryActive) {
                    if (completed) {
                        lifecycleGate.activeSession()?.let { activeSession ->
                            requestConnection(activeSession)
                        }
                    } else {
                        scope.launch {
                            delay(USB_IO_CLEANUP_RETRY_MS)
                            scheduleLateUsbDrain(cleanup)
                        }
                    }
                }
            }
        }
        val installed = lateUsbCleanupSlot.tryInstall(cleanup, reaperJob)
        if (!installed) {
            reaperJob.cancel()
            if (lateUsbCleanupSlot.ownsCleanup(cleanup)) return
            if (scope.coroutineContext[Job]?.isActive == true) {
                scope.launch {
                    delay(USB_IO_CLEANUP_RETRY_MS)
                    scheduleLateUsbDrain(cleanup)
                }
            }
            return
        }
        reaperJob.start()
    }

    private fun setState(update: (BadgeUsbState) -> BadgeUsbState) {
        _state.update(update)
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

    private fun UsbDevice.attachmentIdentity(): BadgeUsbDeviceIdentity =
        BadgeUsbDeviceIdentity(deviceId = deviceId, devicePath = deviceName)

    private fun Intent.usbAttachmentToken(): BadgeUsbAttachmentToken? {
        val generation = getLongExtra(
            EXTRA_USB_PERMISSION_ATTACHMENT_GENERATION,
            NO_ATTACHMENT_GENERATION,
        )
        val deviceId = getIntExtra(EXTRA_USB_PERMISSION_DEVICE_ID, NO_USB_DEVICE_ID)
        val devicePath = getStringExtra(EXTRA_USB_PERMISSION_DEVICE_PATH).orEmpty()
        if (generation < 1L || deviceId == NO_USB_DEVICE_ID || devicePath.isBlank()) return null
        return BadgeUsbAttachmentToken(
            generation = generation,
            identity = BadgeUsbDeviceIdentity(deviceId, devicePath),
        )
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
