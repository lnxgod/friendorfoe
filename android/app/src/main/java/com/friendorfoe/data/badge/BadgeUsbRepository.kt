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
    "skimmer" to BadgeDisplayClassPolicy(true, "both", "near", 88),
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

fun badgeDisplayPolicyCommandJson(
    policy: BadgeDisplayPolicy,
    persist: Boolean = true
): JsonObject = JsonObject().apply {
    addProperty("cmd", "badge_display_policy")
    addProperty("persist", persist)
    add("policy", policy.toJsonObject())
}

fun badgeDisplayNavCommandJson(action: String): JsonObject = JsonObject().apply {
    addProperty("cmd", "display_nav")
    addProperty("action", action)
}

data class BadgeTheme(
    val version: Int = 1,
    val palette: String = "field",
    val background: String = "dark",
    val brightness: Int = 100,
    val accents: Map<String, Int> = defaultBadgeThemeAccents()
) {
    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        addProperty("palette", palette)
        addProperty("background", background)
        addProperty("brightness", brightness.coerceIn(25, 100))
        add("accents", JsonObject().apply {
            accents.forEach { (key, value) ->
                addProperty(key, value.coerceIn(0, 0xffff))
            }
        })
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
        val scanners = obj.getAsJsonArray("scanners")?.mapNotNull { element ->
            runCatching {
                val s = element.asJsonObject
                BadgeScannerStatus(
                    slot = s.badgeOptInt("slot", -1),
                    uart = s.badgeOptString("uart"),
                    connected = s.badgeOptBoolean("connected"),
                    slotRole = s.badgeOptString("slot_role"),
                    expectedScanProfile = s.badgeOptString("expected_scan_profile"),
                    scanProfile = s.badgeOptString("scan_profile"),
                    roleAcked = s.badgeOptBoolean("role_acked"),
                    health = s.badgeOptString("health"),
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

internal fun badgeUsbReaderOwnsSession(
    lifecycleActive: Boolean,
    activeConnectionMatches: Boolean,
): Boolean = lifecycleActive && activeConnectionMatches

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
    val transport: BadgeInvestigationTransport,
    val parser: BadgeInvestigationStreamParser,
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
        private const val NO_LIFECYCLE_SESSION = -1L
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
                    if (!lifecycleGate.isActive(lifecycleSession)) return
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        scope.launch { connectToDevice(device, lifecycleSession) }
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
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> requestConnection()
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
        if (!lifecycleGate.begin()) return
        val lifecycleSession = lifecycleGate.activeSession() ?: return
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
        if (!lifecycleGate.end(lifecycleSession)) return
        disconnect("Badge USB stopped", lifecycleSession)
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
        val lifecycleSession = lifecycleGate.activeSession() ?: return
        refresh(lifecycleSession)
    }

    private fun refresh(lifecycleSession: Long) {
        if (!lifecycleGate.isActive(lifecycleSession)) return
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            setState {
                it.copy(
                    status = BadgeUsbStatus.DISCONNECTED,
                    deviceName = null,
                    message = "Attach a FoF badge over USB-C",
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
                    message = "FoF badge found. USB access required.",
                    transportLabel = "USB-C"
                )
            }
            return
        }

        scope.launch { connectToDevice(device, lifecycleSession) }
    }

    fun requestConnection() {
        val lifecycleSession = lifecycleGate.activeSession() ?: return
        requestConnection(lifecycleSession)
    }

    private fun requestConnection(lifecycleSession: Long) {
        if (!lifecycleGate.isActive(lifecycleSession)) return
        registerReceiverIfNeeded()
        val candidates = findBadgeCandidates()
        if (candidates.isEmpty()) {
            refresh(lifecycleSession)
            return
        }
        if (candidates.size > 1) {
            reportAmbiguousBadgeDevices(candidates)
            return
        }
        val device = candidates.first()
        if (usbManager.hasPermission(device)) {
            scope.launch { connectToDevice(device, lifecycleSession) }
            return
        }

        if (!lifecycleGate.isActive(lifecycleSession)) return

        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_IMMUTABLE else 0
        val permissionIntent = PendingIntent.getBroadcast(
            context,
            lifecycleSession.hashCode(),
            Intent(ACTION_USB_PERMISSION)
                .setPackage(context.packageName)
                .putExtra(EXTRA_USB_PERMISSION_SESSION, lifecycleSession),
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
            } else {
                fetchNetworkStatus(showErrors = true)
            }
        }
    }

    fun investigateBle(request: com.friendorfoe.detection.BleInvestigationRequest): Boolean {
        val transport = when (
            BadgeControlTransportPolicy.select(
                hasUsb = hasUsbCommandPath(),
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
            rememberInvestigationRequestId(request.requestId)
            investigationGeneration++
            ActiveBadgeInvestigation(
                generation = investigationGeneration,
                request = request,
                transport = transport,
                parser = BadgeInvestigationStreamParser(request.requestId),
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
        if (!writeLine("FOF_CTL:${investigationCommand(operation.request)}")) {
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
            investigationCommand(operation.request),
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
            val selection = JsonObject().apply {
                addProperty("cmd", "ble_investigation_chunk")
                addProperty("request_id", operation.request.requestId)
                addProperty("seq", seq)
            }
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
            investigationCommand(operation.request).toString().toByteArray(Charsets.UTF_8),
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
            val selection = JsonObject().apply {
                addProperty("cmd", "ble_investigation_chunk")
                addProperty("request_id", operation.request.requestId)
                addProperty("seq", seq)
            }
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

    private fun investigationCommand(
        request: com.friendorfoe.detection.BleInvestigationRequest,
    ): JsonObject = JsonObject().apply {
        addProperty("cmd", "ble_investigate")
        addProperty("request_id", request.requestId)
        addProperty(
            "mode",
            if (request.target.mode == BleInvestigationMode.GATT) "gatt" else "passive_capture",
        )
        if (request.target.mac == null) add("target", com.google.gson.JsonNull.INSTANCE)
        else addProperty("target", request.target.mac)
        addProperty("timeout_ms", badgeInvestigationTotalTimeoutMs(request.timeoutMs))
    }

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

    fun applyBadgeTheme(theme: BadgeTheme, persist: Boolean = true) {
        sendControl(badgeThemeCommandJson(theme, persist))
    }

    fun resetBadgeTheme(persist: Boolean = true) {
        sendControl(JsonObject().apply {
            addProperty("cmd", "badge_theme_reset")
            addProperty("persist", persist)
        })
    }

    fun displayNav(action: String) {
        sendControl(badgeDisplayNavCommandJson(action))
    }

    @Suppress("UNUSED_PARAMETER")
    fun flashScannerFirmware(
        uart: String,
        name: String,
        version: String,
        firmware: ByteArray,
        forceRelay: Boolean = false
    ) {
        val guidance = BadgeControlTransportPolicy.scannerFirmwareStagingGuidance()
        setState {
            it.copy(
                message = guidance,
                firmwareProgress = BadgeFirmwareProgress(
                    kind = "upload",
                    stage = "disabled",
                    total = firmware.size.toLong(),
                    error = guidance,
                ),
            )
        }
    }

    private fun sendControl(payload: JsonObject) {
        scope.launch {
            when (
                BadgeControlTransportPolicy.select(
                    hasUsb = hasUsbCommandPath(),
                    hasBle = hasBleCommandPath(),
                    hasHttp = activeHttpBaseUrl() != null,
                )
            ) {
                BadgeControlTransport.USB -> {
                    if (usbInvestigationOwnsControlReply()) {
                        setState { it.copy(message = "BLE investigation command is awaiting badge reply") }
                        return@launch
                    }
                    writeLine("FOF_CTL:$payload")
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
                message = "Multiple Espressif USB devices found: $names. Attach only the badge over USB-C.",
                transportLabel = "USB-C"
            )
        }
    }

    private suspend fun connectToDevice(device: UsbDevice, lifecycleSession: Long) {
        connectionMutex.withLock {
            if (!lifecycleGate.isActive(lifecycleSession)) return
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

            if (!lifecycleGate.isActive(lifecycleSession)) {
                runCatching { connection.releaseInterface(port.usbInterface) }
                connection.close()
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
            startReader(connection, port.inEndpoint, device.displayName(), lifecycleSession)
            startUsbStatusPoller()
        }
    }

    private fun startReader(
        connection: android.hardware.usb.UsbDeviceConnection,
        inEndpoint: UsbEndpoint,
        deviceName: String,
        lifecycleSession: Long,
    ) {
        readJob?.cancel()
        readJob = scope.launch {
            val buffer = ByteArray(256)
            val lineFramer = BadgeUsbLineFramer(
                onLine = { bytes, length ->
                    decodeBadgeUtf8(bytes, length)?.let(::handleLine)
                        ?: Log.w(TAG, "Dropping malformed UTF-8 badge line")
                },
                onOverlongLine = {
                    Log.w(TAG, "Dropping overlong badge line")
                },
            )
            try {
                while (isActive) {
                    val read = connection.bulkTransfer(
                        inEndpoint,
                        buffer,
                        buffer.size,
                        READ_TIMEOUT_MS
                    )
                    if (read > 0) {
                        lineFramer.accept(buffer, read)
                    } else {
                        delay(25)
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (e: Exception) {
                Log.w(TAG, "Badge USB reader stopped", e)
                if (!badgeUsbReaderOwnsSession(
                        lifecycleActive = lifecycleGate.isActive(lifecycleSession),
                        activeConnectionMatches = activeConnection === connection,
                    )
                ) {
                    return@launch
                }
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

    private suspend fun writeLine(line: String): Boolean = usbWriteMutex.withLock {
        withContext(Dispatchers.IO) {
            val connection = activeConnection ?: return@withContext false
            val out = activeOutEndpoint ?: return@withContext false
            val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
            connection.bulkTransfer(out, bytes, bytes.size, WRITE_TIMEOUT_MS) == bytes.size
        }
    }

    private fun hasUsbCommandPath(): Boolean {
        return state.value.status == BadgeUsbStatus.CONNECTED &&
            activeConnection != null &&
            activeOutEndpoint != null
    }

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

    private fun handleLine(line: String) {
        val trimmed = line.trim()
        if (trimmed.isEmpty()) return
        val receivedAtElapsedMs = elapsedRealtimeMs()
        val investigationHandled = if (line.startsWith("FOF_INV:")) {
            handleUsbInvestigationLine(line)
        } else {
            handleUsbInvestigationLine(trimmed)
        }

        val detection = if (trimmed.startsWith("FOF_DET:")) {
            parseDetection(trimmed.removePrefix("FOF_DET:"), receivedAtElapsedMs)
        } else {
            null
        }
        val status = if (trimmed.startsWith("FOF_STATUS:")) {
            parseBadgeControlStatus(
                trimmed.removePrefix("FOF_STATUS:"),
                snapshotAtElapsedMs = receivedAtElapsedMs,
            )
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
        val activity = badgeUsbActivityForLine(
            line = trimmed,
            receivedAtElapsedMs = receivedAtElapsedMs,
            detection = detection,
            status = status,
            firmwareProgress = firmwareProgress,
            investigationHandled = investigationHandled,
        )

        setState { current ->
            val nextDetections = detection?.let {
                (listOf(it) + current.detections).take(MAX_RECENT_DETECTIONS)
            } ?: current.detections

            current.copy(
                lastLine = trimmed.take(160),
                eventCount = if (detection != null) current.eventCount + 1 else current.eventCount,
                detections = nextDetections,
                activity = activity?.let { pushBadgeUsbActivity(current.activity, it) }
                    ?: current.activity,
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
                    investigationHandled -> "Badge investigation updated"
                    detection != null -> "Receiving badge events"
                    else -> current.message
                }
            )
        }
    }

    private fun handleUsbInvestigationLine(line: String): Boolean {
        val operation = synchronized(investigationLock) {
            activeInvestigation?.takeIf { it.transport == BadgeInvestigationTransport.USB }
        } ?: return false
        if (line.startsWith("FOF_INV:")) {
            return acceptInvestigationLine(operation, line).accepted
        }
        val ack = operation.ackGate.accept(
            line = line,
            parsed = null,
            ownsControlReply = !operation.controlAck.isCompleted,
        ) ?: return false
        operation.controlAck.complete(ack)
        return true
    }

    private fun usbInvestigationOwnsControlReply(): Boolean = synchronized(investigationLock) {
        activeInvestigation?.let {
            it.transport == BadgeInvestigationTransport.USB && !it.controlAck.isCompleted
        } == true
    }

    private fun parseDetection(
        json: String,
        receivedAtElapsedMs: Long,
    ): BadgeUsbDetection? {
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
                rssi = obj.get("rssi")?.asInt ?: 0,
                receivedAtElapsedMs = receivedAtElapsedMs,
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

    private fun disconnect(reason: String, lifecycleSession: Long? = null) {
        scope.launch {
            connectionMutex.withLock {
                if (lifecycleSession != null && !lifecycleGate.canClean(lifecycleSession)) {
                    return@withLock
                }
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
        terminateInvestigationTransport(BadgeInvestigationTransport.USB)
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
