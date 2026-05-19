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
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
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
    val score: Int,
    val confidencePct: Int = 0,
    val evidenceQuality: Int = 0,
    val displayRank: Int = 0,
    val ageSeconds: Int,
    val lastSeenSeconds: Int = 0,
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
    BadgeDisplayPolicyClassInfo("scanner_status", "Scanner Status")
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
    "scanner_status" to BadgeDisplayClassPolicy(true, "lower", "present", 10)
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
    val message: String = "Connect a FoF badge over USB-C",
    val transportLabel: String = "",
    val lastLine: String? = null,
    val eventCount: Int = 0,
    val detections: List<BadgeUsbDetection> = emptyList(),
    val controlStatus: BadgeControlStatus? = null,
    val firmwareProgress: BadgeFirmwareProgress? = null
)

internal fun parseBadgeControlStatus(json: String): BadgeControlStatus? {
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
                    score = e.badgeOptInt("score"),
                    confidencePct = e.badgeOptInt("confidence_pct"),
                    evidenceQuality = e.badgeOptInt("evidence_quality"),
                    displayRank = e.badgeOptInt("display_rank"),
                    ageSeconds = e.badgeOptInt("age_s"),
                    lastSeenSeconds = e.badgeOptInt("last_seen_s"),
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
            parseBadgeControlStatus(trimmed.removePrefix("FOF_STATUS:"))
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
                    trimmed.startsWith("FOF_PONG:") -> "Badge replied ${trimmed.removePrefix("FOF_PONG:")}"
                    status != null -> "Badge status updated"
                    firmwareProgress != null -> firmwareProgress.error.ifBlank {
                        "Firmware ${firmwareProgress.kind} ${firmwareProgress.stage} ${firmwareProgress.percent}%"
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
