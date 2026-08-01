package com.friendorfoe.data.badge

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

data class BadgeConfigReadback<T>(
    val value: T?,
    val hash: Long?,
    val issue: String?
) {
    val isEditable: Boolean
        get() = value != null && hash != null && hash != 0L && issue == null
}

data class BadgeNetworkModeReadback(
    val value: BadgeNetworkMode?,
    val issue: String?
) {
    val isEditable: Boolean
        get() = value != null && issue == null
}

data class BadgeDebugBridgeEvidence(
    val physicalSerialPort: String?,
    val physicalResponseAtElapsedMs: Long?,
    val lastError: String?
)

data class BadgeControlStatus(
    val version: String,
    val receivedAtElapsedMs: Long,
    val themeReadback: BadgeConfigReadback<BadgeTheme>,
    val policyReadback: BadgeConfigReadback<BadgeDisplayPolicy>,
    val networkModeReadback: BadgeNetworkModeReadback,
    val entities: List<BadgeThreatEntity>,
    val scanners: List<BadgeScannerStatus>,
    val displayState: BadgeDisplayState?,
    val debugBridge: BadgeDebugBridgeEvidence?,
    val reporting: BadgeReportingStatus,
    val counts: BadgeThreatCounts,
    val bleControl: BadgeBleControlStatus,
    val safeMode: Boolean,
    val safeReason: String,
    val resetReason: String,
    val crashCount: Int,
    val recoveryMode: String,
    val stackFreeBytes: Map<String, Int>,
    val heapInternalFreeBytes: Long,
    val heapInternalMinimumFreeBytes: Long,
    val psramFreeBytes: Long,
    val threatScore: Float = 0f,
    val colorRgb565: Int = 0,
    val filteredCounts: Map<String, Int> = emptyMap(),
    val resetReasonCode: Long = 0,
    val resetExpected: Boolean = false,
    val usbControlAgeSeconds: Long? = null,
    val heapInternalLargestBytes: Long = 0,
    val psramTotalBytes: Long = 0,
    val psramLargestBytes: Long = 0
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
