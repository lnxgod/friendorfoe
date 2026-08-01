package com.friendorfoe.data.badge

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonPrimitive
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant

internal fun parseBadgeControlStatus(
    json: String,
    receivedAtElapsedMs: Long,
    receivedAtWallClock: Instant,
): BadgeControlStatus? {
    return runCatching {
        val element = JsonParser.parseString(json)
        if (!element.isJsonObject) return null
        val obj = element.asJsonObject
        val version = obj.badgeStrictStringOrNull("version")
            ?.takeIf { it.isNotBlank() }
            ?: return null

        val reportingObj = obj.badgeObjectOrNull("reporting")
        val countsObj = obj.badgeObjectOrNull("counts")
        val themeReadback = parseBadgeThemeReadback(
            obj.badgeObjectOrNull("theme"),
            obj.badgeFirmwareHashOrNull("theme_hash")
        )
        val policyReadback = parseBadgePolicyReadback(
            obj.badgeObjectOrNull("display_policy"),
            obj.badgeFirmwareHashOrNull("display_policy_hash")
        )
        val debugBridge = parseBadgeDebugBridgeEvidence(
            obj.badgeObjectOrNull("debug_bridge"),
            receivedAtElapsedMs,
        )
        BadgeControlStatus(
            version = version,
            receivedAtElapsedMs = receivedAtElapsedMs,
            receivedAtWallClock = receivedAtWallClock,
            themeReadback = themeReadback,
            policyReadback = policyReadback,
            networkModeReadback = parseBadgeNetworkModeReadback(obj),
            entities = parseBadgeEntities(obj),
            scanners = parseBadgeScanners(obj),
            displayState = parseBadgeDisplayState(obj.badgeObjectOrNull("display_state")),
            debugBridge = debugBridge,
            reporting = BadgeReportingStatus(
                networkMode = reportingObj?.badgeOptString("network_mode")
                    ?.takeIf { it.isNotBlank() }
                    ?: obj.badgeOptString("network_mode").ifBlank { "off" },
                backendEnabled = reportingObj?.badgeOptBoolean("backend_enabled")
                    ?: obj.badgeOptBoolean("backend_enabled"),
                networkTtlSeconds = reportingObj?.badgeOptInt("network_ttl_s")
                    ?: obj.badgeOptInt("network_ttl_s"),
                wifiSta = reportingObj?.badgeOptBoolean("wifi_sta")
                    ?: obj.badgeOptBoolean("wifi_sta"),
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
            bleControl = parseBadgeBleControlStatus(obj.badgeObjectOrNull("ble_control")),
            safeMode = obj.badgeOptBoolean("safe_mode"),
            safeReason = obj.badgeOptString("safe_reason"),
            resetReason = obj.badgeOptString("reset_reason")
                .ifBlank { reportingObj?.badgeOptString("reset_reason").orEmpty() },
            crashCount = obj.badgeOptInt("crash_count").takeIf { it != 0 }
                ?: reportingObj?.badgeOptInt("crash_count")
                ?: 0,
            recoveryMode = obj.badgeOptString("recovery_mode")
                .ifBlank { reportingObj?.badgeOptString("recovery_mode").orEmpty() },
            stackFreeBytes = linkedMapOf(
                "main" to obj.badgeIntWithReportingFallback("stack_main_free", reportingObj),
                "display" to obj.badgeIntWithReportingFallback("stack_display_free", reportingObj),
                "usb" to obj.badgeIntWithReportingFallback("stack_usb_free", reportingObj),
                "uart_ble" to obj.badgeIntWithReportingFallback("stack_uart_ble_free", reportingObj),
                "uart_wifi" to obj.badgeIntWithReportingFallback("stack_uart_wifi_free", reportingObj)
            ),
            heapInternalFreeBytes = obj.badgeLongWithReportingFallback(
                "heap_internal_free",
                reportingObj
            ),
            heapInternalMinimumFreeBytes = obj.badgeLongWithReportingFallback(
                "heap_internal_min_free",
                reportingObj
            ),
            psramFreeBytes = obj.badgeLongWithReportingFallback("psram_free", reportingObj),
            threatScore = obj.badgeOptFloat("threat_score"),
            colorRgb565 = obj.badgeOptInt("color_rgb565"),
            filteredCounts = parseBadgeIntMap(obj.badgeObjectOrNull("filtered_counts")),
            resetReasonCode = obj.badgeOptLong("reset_reason_code").takeIf { it != 0L }
                ?: reportingObj?.badgeOptLong("reset_reason_code")
                ?: 0L,
            resetExpected = if (obj.has("reset_expected")) {
                obj.badgeOptBoolean("reset_expected")
            } else {
                reportingObj?.badgeOptBoolean("reset_expected") ?: false
            },
            usbControlAgeSeconds = obj.badgeOptLongOrNull("usb_control_age_s")
                ?: reportingObj?.badgeOptLongOrNull("usb_control_age_s"),
            heapInternalLargestBytes = obj.badgeLongWithReportingFallback(
                "heap_internal_largest",
                reportingObj
            ),
            psramTotalBytes = obj.badgeLongWithReportingFallback("psram_total", reportingObj),
            psramLargestBytes = obj.badgeLongWithReportingFallback("psram_largest", reportingObj)
        )
    }.getOrNull()
}

private fun parseBadgeThemeReadback(
    obj: JsonObject?,
    hash: Long?
): BadgeConfigReadback<BadgeTheme> {
    if (obj == null) return BadgeConfigReadback(null, hash, "Theme readback is missing")
    if (hash == null || hash !in 1..0xFFFF_FFFFL) {
        return BadgeConfigReadback(null, hash, "Theme hash is missing or unknown")
    }
    val parsed = runCatching {
        require(
            obj.keySet() == setOf(
                "version",
                "palette",
                "background",
                "brightness",
                "accents"
            )
        ) { "Theme object shape is invalid" }
        val accentsObj = obj.badgeRequiredObject("accents")
        require(accentsObj.keySet() == BadgeTheme.accentOrder.toSet()) {
            "Theme accent set is incomplete"
        }
        val theme = BadgeTheme(
            version = obj.badgeRequiredInt("version"),
            palette = obj.badgeRequiredString("palette"),
            background = obj.badgeRequiredString("background"),
            intensity = obj.badgeRequiredInt("brightness"),
            accents = linkedMapOf<String, Int>().apply {
                BadgeTheme.accentOrder.forEach { key ->
                    put(key, accentsObj.badgeRequiredInt(key))
                }
            }
        )
        BadgeTheme.validate(theme).getOrThrow()
    }
    val theme = parsed.getOrElse { error ->
        return BadgeConfigReadback(null, hash, error.message ?: "Theme readback is invalid")
    }
    if (theme.firmwareHash() != hash) {
        return BadgeConfigReadback(null, hash, "Theme hash does not match the full readback")
    }
    return BadgeConfigReadback(theme, hash, null)
}

private fun parseBadgePolicyReadback(
    obj: JsonObject?,
    hash: Long?
): BadgeConfigReadback<BadgeDisplayPolicy> {
    if (obj == null) return BadgeConfigReadback(null, hash, "Display policy readback is missing")
    if (hash == null || hash !in 1..0xFFFF_FFFFL) {
        return BadgeConfigReadback(null, hash, "Display policy hash is missing or unknown")
    }
    val parsed = runCatching {
        require(obj.keySet() == setOf("version", "classes")) {
            "Display policy object shape is invalid"
        }
        val classesObj = obj.badgeRequiredObject("classes")
        require(classesObj.keySet() == BadgeDisplayPolicy.classOrder.toSet()) {
            "Display policy class set is incomplete"
        }
        val policy = BadgeDisplayPolicy(
            version = obj.badgeRequiredInt("version"),
            classes = linkedMapOf<String, BadgeDisplayRule>().apply {
                BadgeDisplayPolicy.classOrder.forEach { key ->
                    val row = classesObj.badgeRequiredObject(key)
                    require(
                        row.keySet() == setOf(
                            "enabled",
                            "lane",
                            "min_proximity",
                            "priority"
                        )
                    ) { "Display policy row shape is invalid: $key" }
                    put(
                        key,
                        BadgeDisplayRule(
                            enabled = row.badgeRequiredBoolean("enabled"),
                            lane = row.badgeRequiredEnum("lane", BadgeDisplayLane.entries) {
                                it.wireValue
                            },
                            minProximity = row.badgeRequiredEnum(
                                "min_proximity",
                                BadgeMinimumProximity.entries
                            ) { it.wireValue },
                            priority = row.badgeRequiredInt("priority")
                        )
                    )
                }
            }
        )
        BadgeDisplayPolicy.validate(policy).getOrThrow()
    }
    val policy = parsed.getOrElse { error ->
        return BadgeConfigReadback(
            null,
            hash,
            error.message ?: "Display policy readback is invalid"
        )
    }
    if (policy.firmwareHash() != hash) {
        return BadgeConfigReadback(
            null,
            hash,
            "Display policy hash does not match the full readback"
        )
    }
    return BadgeConfigReadback(policy, hash, null)
}

private fun parseBadgeNetworkModeReadback(obj: JsonObject): BadgeNetworkModeReadback {
    val raw = obj.badgeStrictStringOrNull("mode")
        ?: return BadgeNetworkModeReadback(null, "Persisted badge mode is missing")
    val value = BadgeNetworkMode.entries.firstOrNull { it.wireValue == raw }
        ?: return BadgeNetworkModeReadback(null, "Persisted badge mode is unknown")
    return BadgeNetworkModeReadback(value, null)
}

private fun parseBadgeDebugBridgeEvidence(
    obj: JsonObject?,
    receivedAtElapsedMs: Long
): BadgeDebugBridgeEvidence? {
    if (obj == null) return null
    val ageMs = obj.badgeStatusAgeMsOrNull("status_age_s")
    val responseAt = ageMs?.let { age ->
        runCatching { Math.subtractExact(receivedAtElapsedMs, age) }.getOrNull()
    }
    return BadgeDebugBridgeEvidence(
        physicalSerialPort = obj.badgeStrictStringOrNull("serial_port")
            ?.takeIf { it.isNotBlank() },
        physicalResponseAtElapsedMs = responseAt,
        lastError = obj.badgeStrictStringOrNull("last_error")
    )
}

private fun parseBadgeEntities(obj: JsonObject): List<BadgeThreatEntity> {
    val array = runCatching { obj.getAsJsonArray("entities") }.getOrNull() ?: return emptyList()
    return array.mapNotNull { element ->
        runCatching {
            val entity = element.asJsonObject
            BadgeThreatEntity(
                label = entity.badgeOptString("label"),
                detail = entity.badgeOptString("detail"),
                evidence = entity.badgeOptString("evidence"),
                threatClass = entity.badgeOptString("class"),
                category = entity.badgeOptString("category"),
                code = entity.badgeOptString("code"),
                displayId = entity.badgeOptString("display_id"),
                source = entity.badgeOptString("source"),
                sourceId = entity.badgeOptInt("source_id"),
                ssid = entity.badgeOptString("ssid"),
                bssid = entity.badgeOptString("bssid"),
                authMode = entity.badgeOptInt("auth_m", -1),
                freqMhz = entity.badgeOptInt("freq_mhz"),
                score = entity.badgeOptInt("score"),
                confidencePct = entity.badgeOptInt("confidence_pct"),
                evidenceQuality = entity.badgeOptInt("evidence_quality"),
                displayRank = entity.badgeOptInt("display_rank"),
                ageSeconds = entity.badgeOptInt("age_s"),
                lastSeenSeconds = entity.badgeOptInt("last_seen_s"),
                rssi = entity.badgeOptInt("rssi"),
                bestRssi = entity.badgeOptInt("best_rssi"),
                events = entity.badgeOptInt("events"),
                seenCount = entity.badgeOptInt("seen_count"),
                groupCount = entity.badgeOptInt("group_count"),
                proximityLevel = entity.badgeOptInt("proximity_level"),
                stale = entity.badgeOptBoolean("stale"),
                lat = entity.badgeOptDoubleOrNull("lat"),
                lon = entity.badgeOptDoubleOrNull("lon"),
                altitudeM = entity.badgeOptFloatOrNull("altitude_m"),
                operatorLat = entity.badgeOptDoubleOrNull("operator_lat"),
                operatorLon = entity.badgeOptDoubleOrNull("operator_lon"),
                operatorId = entity.badgeOptString("operator_id").ifBlank { null }
            )
        }.getOrNull()
    }
}

private fun parseBadgeScanners(obj: JsonObject): List<BadgeScannerStatus> {
    val array = runCatching { obj.getAsJsonArray("scanners") }.getOrNull() ?: return emptyList()
    return array.mapNotNull { element ->
        runCatching {
            val scanner = element.asJsonObject
            BadgeScannerStatus(
                slot = scanner.badgeOptInt("slot", -1),
                uart = scanner.badgeOptString("uart"),
                connected = scanner.badgeOptBoolean("connected"),
                slotRole = scanner.badgeOptString("slot_role"),
                expectedScanProfile = scanner.badgeOptString("expected_scan_profile"),
                scanProfile = scanner.badgeOptString("scan_profile"),
                roleAcked = scanner.badgeOptBoolean("role_acked"),
                health = scanner.badgeOptString("health"),
                uartRawSeen = scanner.badgeOptBoolean("uart_raw_seen"),
                uartRawAgeSeconds = scanner.badgeOptLongOrNull("uart_raw_age_s"),
                uartJsonErrors = scanner.badgeOptInt("uart_json_err"),
                commandRx = scanner.badgeOptInt("cmd_rx"),
                commandLastAgeSeconds = scanner.badgeOptLongOrNull("cmd_last_age_s"),
                bleAdvSeen = scanner.badgeOptInt("ble_adv_seen"),
                bleFpEmit = scanner.badgeOptInt("ble_fp_emit"),
                bleMetaSeen = scanner.badgeOptInt("ble_meta_seen"),
                bleTrackerSeen = scanner.badgeOptInt("ble_tracker_seen"),
                ridEmit = scanner.badgeOptInt("rid_emit"),
                privacySeen = scanner.badgeOptInt("privacy_seen"),
                wifiTotalFrames = scanner.badgeOptInt("wifi_total_frames"),
                wifiDroneSsidEmit = scanner.badgeOptInt("wifi_drone_ssid_emit"),
                wifiNotableSsidEmit = scanner.badgeOptInt("wifi_notable_ssid_emit"),
                wifiLastDroneSsid = scanner.badgeOptString("wifi_last_drone_ssid"),
                wifiLastNotableSsid = scanner.badgeOptString("wifi_last_notable_ssid"),
                displayPolicyHash = scanner.badgeOptLong("display_policy_hash"),
                displayPolicyAckHash = scanner.badgeOptLong("display_policy_ack_hash"),
                filteredCounts = parseBadgeIntMap(
                    scanner.badgeObjectOrNull("filtered_counts")
                ),
                firmwareState = scanner.badgeOptString("fw_state"),
                targetVersion = scanner.badgeOptString("target_ver"),
                otaState = scanner.badgeOptString("ota_state"),
                lastFirmwareError = scanner.badgeOptString("last_fw_error")
            )
        }.getOrNull()
    }
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

private fun parseBadgeIntMap(obj: JsonObject?): Map<String, Int> {
    if (obj == null) return emptyMap()
    return obj.entrySet().associate { (key, value) ->
        key to runCatching { value.asInt }.getOrDefault(0)
    }
}

private fun JsonObject.badgeObjectOrNull(key: String): JsonObject? =
    runCatching { getAsJsonObject(key) }.getOrNull()

private fun JsonObject.badgeRequiredObject(key: String): JsonObject =
    badgeObjectOrNull(key) ?: error("Missing object: $key")

private fun JsonObject.badgeRequiredString(key: String): String =
    badgeStrictStringOrNull(key) ?: error("Missing or invalid string: $key")

private fun JsonObject.badgeRequiredInt(key: String): Int {
    val value = get(key) as? JsonPrimitive
    require(value != null && value.isNumber) { "Missing or invalid integer: $key" }
    val wireNumber = value.toString()
    require(wireNumber.matches(Regex("-?\\d+"))) { "Invalid integer: $key" }
    return wireNumber.toIntOrNull() ?: error("Integer is outside the supported range: $key")
}

private fun JsonObject.badgeRequiredBoolean(key: String): Boolean {
    val value = get(key) as? JsonPrimitive
    require(value != null && value.isBoolean) { "Missing or invalid Boolean: $key" }
    return value.asBoolean
}

private fun <T> JsonObject.badgeRequiredEnum(
    key: String,
    values: Iterable<T>,
    wireValue: (T) -> String
): T {
    val raw = badgeRequiredString(key)
    return values.firstOrNull { wireValue(it) == raw } ?: error("Unknown $key: $raw")
}

private fun JsonObject.badgeStrictStringOrNull(key: String): String? {
    val value = get(key) as? JsonPrimitive ?: return null
    if (!value.isString) return null
    return value.asString
}

private fun JsonObject.badgeFirmwareHashOrNull(key: String): Long? {
    val value = get(key) as? JsonPrimitive ?: return null
    if (!value.isNumber) return null
    val wireNumber = value.toString()
    if (!wireNumber.matches(Regex("\\d+"))) return null
    return wireNumber.toLongOrNull()?.takeIf { it in 1..0xFFFF_FFFFL }
}

private fun JsonObject.badgeStatusAgeMsOrNull(key: String): Long? {
    val value = get(key) as? JsonPrimitive ?: return null
    if (!value.isNumber) return null
    val wireNumber = value.toString()
    val seconds = wireNumber.toDoubleOrNull() ?: return null
    if (!seconds.isFinite() || seconds < 0.0) return null
    val milliseconds = runCatching {
        BigDecimal(wireNumber)
            .movePointRight(3)
            .setScale(0, RoundingMode.HALF_UP)
    }.getOrNull() ?: return null
    if (milliseconds < BigDecimal.ZERO || milliseconds > BigDecimal.valueOf(Long.MAX_VALUE)) {
        return null
    }
    return runCatching { milliseconds.longValueExact() }.getOrNull()
}

private fun JsonObject.badgeOptString(key: String): String =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asString.orEmpty() }.getOrDefault("")

private fun JsonObject.badgeOptInt(key: String, fallback: Int = 0): Int =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asInt ?: fallback }.getOrDefault(fallback)

private fun JsonObject.badgeOptLong(key: String, fallback: Long = 0L): Long =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong ?: fallback }.getOrDefault(fallback)

private fun JsonObject.badgeOptLongOrNull(key: String): Long? =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asLong }.getOrNull()

private fun JsonObject.badgeOptFloat(key: String, fallback: Float = 0f): Float =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat ?: fallback }.getOrDefault(fallback)

private fun JsonObject.badgeOptFloatOrNull(key: String): Float? =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asFloat }.getOrNull()

private fun JsonObject.badgeOptDoubleOrNull(key: String): Double? =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asDouble }.getOrNull()

private fun JsonObject.badgeOptBoolean(key: String, fallback: Boolean = false): Boolean =
    runCatching { get(key)?.takeIf { !it.isJsonNull }?.asBoolean ?: fallback }
        .getOrDefault(fallback)

private fun JsonObject.badgeIntWithReportingFallback(
    key: String,
    reporting: JsonObject?
): Int = badgeOptInt(key).takeIf { it != 0 } ?: reporting?.badgeOptInt(key) ?: 0

private fun JsonObject.badgeLongWithReportingFallback(
    key: String,
    reporting: JsonObject?
): Long = badgeOptLong(key).takeIf { it != 0L } ?: reporting?.badgeOptLong(key) ?: 0L
