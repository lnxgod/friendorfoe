package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlStatusParserTest {

    @Test
    fun parsesCompleteStrictReadbacksAndExtendedStatus() {
        val status = parseBadgeControlStatus(
            validStatusJson(),
            receivedAtElapsedMs = 50_000L
        )

        assertNotNull(status)
        status!!
        assertEquals("0.64.65", status.version)
        assertEquals(50_000L, status.receivedAtElapsedMs)
        assertTrue(status.themeReadback.isEditable)
        assertEquals(0xC3AA2A8DL, status.themeReadback.hash)
        assertEquals("field", status.themeReadback.value?.palette)
        assertEquals(100, status.themeReadback.value?.intensity)
        assertTrue(status.policyReadback.isEditable)
        assertEquals(0x0DAD6299L, status.policyReadback.hash)
        assertEquals(
            BadgeMinimumProximity.CLOSE,
            status.policyReadback.value?.classes?.get("hid")?.minProximity
        )
        assertEquals(BadgeNetworkMode.USB_ONLY, status.networkModeReadback.value)
        assertEquals("off", status.reporting.networkMode)
        assertEquals(2, status.counts.drone)
        assertEquals("FLOCK CAM", status.entities.single().label)
        assertEquals("ble_primary", status.scanners.single().scanProfile)
        assertTrue(status.displayState?.detailMode == true)
        assertTrue(status.bleControl.bonded)
        assertTrue(status.bleControl.encrypted)
        assertTrue(status.safeMode)
        assertEquals("PANIC", status.resetReason)
        assertEquals(2, status.crashCount)
        assertEquals("safe_usb", status.recoveryMode)
        assertEquals(4096, status.stackFreeBytes["main"])
        assertEquals(123456L, status.heapInternalFreeBytes)
        assertEquals(65432L, status.heapInternalMinimumFreeBytes)
        assertEquals(7340032L, status.psramFreeBytes)
        assertEquals("/dev/cu.usbmodem1101", status.debugBridge?.physicalSerialPort)
        assertEquals(48_750L, status.debugBridge?.physicalResponseAtElapsedMs)
        assertEquals("", status.debugBridge?.lastError)
    }

    @Test
    fun blankOrMissingVersionIsNotValidStatus() {
        assertNull(parseBadgeControlStatus("{}", receivedAtElapsedMs = 10L))
        assertNull(
            parseBadgeControlStatus(
                "{\"version\":\"\"}",
                receivedAtElapsedMs = 10L
            )
        )
        assertNull(
            parseBadgeControlStatus(
                "{\"version\":64}",
                receivedAtElapsedMs = 10L
            )
        )
    }

    @Test
    fun missingThemeFieldsNeverBecomeEditableDefaults() {
        val status = parseBadgeControlStatus(
            validStatusJson(
                themeHash = 1L,
                themeJson = """{"version":1}"""
            ),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(status.themeReadback.isEditable)
        assertNull(status.themeReadback.value)
        assertNotNull(status.themeReadback.issue)
        assertTrue(status.policyReadback.isEditable)
    }

    @Test
    fun zeroHashesRemainUnknown() {
        val status = parseBadgeControlStatus(
            validStatusJson(themeHash = 0L, policyHash = 0L),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(status.themeReadback.isEditable)
        assertNull(status.themeReadback.value)
        assertFalse(status.policyReadback.isEditable)
        assertNull(status.policyReadback.value)
    }

    @Test
    fun persistedModeWinsOverRuntimeNetworkOff() {
        val status = parseBadgeControlStatus(
            validStatusJson(mode = "usb_only", runtimeNetworkMode = "off"),
            receivedAtElapsedMs = 10L
        )!!

        assertEquals(BadgeNetworkMode.USB_ONLY, status.networkModeReadback.value)
        assertTrue(status.networkModeReadback.isEditable)
        assertEquals("off", status.reporting.networkMode)
    }

    @Test
    fun missingOrUnknownPersistedModeNeverBecomesDefault() {
        val missing = parseBadgeControlStatus(
            validStatusJson(mode = null),
            receivedAtElapsedMs = 10L
        )!!
        val unknown = parseBadgeControlStatus(
            validStatusJson(mode = "mesh_future"),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(missing.networkModeReadback.isEditable)
        assertNull(missing.networkModeReadback.value)
        assertFalse(unknown.networkModeReadback.isEditable)
        assertNull(unknown.networkModeReadback.value)
    }

    @Test
    fun hashMismatchInvalidatesOnlyThatConfigurationReadback() {
        val themeMismatch = parseBadgeControlStatus(
            validStatusJson(themeHash = 1L),
            receivedAtElapsedMs = 10L
        )!!
        val policyMismatch = parseBadgeControlStatus(
            validStatusJson(policyHash = 1L),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(themeMismatch.themeReadback.isEditable)
        assertNull(themeMismatch.themeReadback.value)
        assertTrue(themeMismatch.policyReadback.isEditable)
        assertFalse(policyMismatch.policyReadback.isEditable)
        assertNull(policyMismatch.policyReadback.value)
        assertTrue(policyMismatch.themeReadback.isEditable)
        assertEquals("FLOCK CAM", policyMismatch.entities.single().label)
    }

    @Test
    fun invalidPolicyEnumDoesNotCoerceToFirmwareDefaults() {
        val invalidPolicy = completePolicyJson.replace(
            "\"lane\":\"both\"",
            "\"lane\":\"sideways\""
        )

        val status = parseBadgeControlStatus(
            validStatusJson(policyJson = invalidPolicy),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(status.policyReadback.isEditable)
        assertNull(status.policyReadback.value)
        assertTrue(status.themeReadback.isEditable)
    }

    @Test
    fun wrongPrimitiveTypesInvalidateOnlyTheirReadback() {
        val stringBrightness = completeThemeJson.replace(
            "\"brightness\":100",
            "\"brightness\":\"100\""
        )
        val stringEnabled = completePolicyJson.replace(
            "\"enabled\":true",
            "\"enabled\":\"true\""
        )

        val themeInvalid = parseBadgeControlStatus(
            validStatusJson(themeJson = stringBrightness),
            receivedAtElapsedMs = 10L
        )!!
        val policyInvalid = parseBadgeControlStatus(
            validStatusJson(policyJson = stringEnabled),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(themeInvalid.themeReadback.isEditable)
        assertTrue(themeInvalid.policyReadback.isEditable)
        assertFalse(policyInvalid.policyReadback.isEditable)
        assertTrue(policyInvalid.themeReadback.isEditable)
        assertEquals("FLOCK CAM", policyInvalid.entities.single().label)
    }

    @Test
    fun fractionalFirmwareIntegersAreRejectedInsteadOfTruncated() {
        val fractionalBrightness = completeThemeJson.replace(
            "\"brightness\":100",
            "\"brightness\":100.5"
        )
        val fractionalPriority = completePolicyJson.replace(
            "\"priority\":100",
            "\"priority\":100.5"
        )

        val themeInvalid = parseBadgeControlStatus(
            validStatusJson(themeJson = fractionalBrightness),
            receivedAtElapsedMs = 10L
        )!!
        val policyInvalid = parseBadgeControlStatus(
            validStatusJson(policyJson = fractionalPriority),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(themeInvalid.themeReadback.isEditable)
        assertFalse(policyInvalid.policyReadback.isEditable)
    }

    @Test
    fun extraThemeOrPolicyKeysNeverBecomeEditable() {
        val extraAccent = completeThemeJson.replace(
            "\"clear\":12133",
            "\"clear\":12133,\"future\":1"
        )
        val extraClass = completePolicyJson.replace(
            "\"scanner_status\":{",
            "\"future\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":1},\"scanner_status\":{"
        )

        val themeInvalid = parseBadgeControlStatus(
            validStatusJson(themeJson = extraAccent),
            receivedAtElapsedMs = 10L
        )!!
        val policyInvalid = parseBadgeControlStatus(
            validStatusJson(policyJson = extraClass),
            receivedAtElapsedMs = 10L
        )!!

        assertFalse(themeInvalid.themeReadback.isEditable)
        assertFalse(policyInvalid.policyReadback.isEditable)
    }

    @Test
    fun debugBridgeLastErrorPreservesMissingVersusBlank() {
        val blank = parseBadgeControlStatus(
            validStatusJson(),
            receivedAtElapsedMs = 10_000L
        )!!
        val missing = parseBadgeControlStatus(
            validStatusJson().replace(
                "\"serial_port\":\"/dev/cu.usbmodem1101\",\"status_age_s\":1.25,\"last_error\":\"\"",
                "\"serial_port\":\"/dev/cu.usbmodem1101\",\"status_age_s\":1.25"
            ),
            receivedAtElapsedMs = 10_000L
        )!!
        val wrongPrimitive = parseBadgeControlStatus(
            validStatusJson().replace("\"last_error\":\"\"", "\"last_error\":false"),
            receivedAtElapsedMs = 10_000L
        )!!

        assertEquals("", blank.debugBridge?.lastError)
        assertNull(missing.debugBridge?.lastError)
        assertNull(wrongPrimitive.debugBridge?.lastError)
    }

    private fun validStatusJson(
        mode: String? = "usb_only",
        runtimeNetworkMode: String = "off",
        themeHash: Long = 0xC3AA2A8DL,
        policyHash: Long = 0x0DAD6299L,
        themeJson: String = completeThemeJson,
        policyJson: String = completePolicyJson
    ): String {
        val modeProperty = mode?.let { "\"mode\":\"$it\"," }.orEmpty()
        return """
            {
              "version":"0.64.65",
              $modeProperty
              "network_mode":"$runtimeNetworkMode",
              "reporting":{
                "network_mode":"$runtimeNetworkMode",
                "backend_enabled":false,
                "network_ttl_s":20,
                "wifi_sta":false,
                "standalone":true,
                "uploads_ok":7,
                "uploads_fail":1,
                "last_upload_age_s":4
              },
              "theme_hash":$themeHash,
              "theme":$themeJson,
              "display_policy_hash":$policyHash,
              "display_policy":$policyJson,
              "filtered_counts":{"beacon":12,"scanner_status":3},
              "counts":{"drone":2,"meta":1,"tracker":1,"wifi_anomaly":1,"ble":3,"other":4},
              "display_state":{
                "active":true,"detail_mode":true,"detail_page":2,
                "focus_index":1,"focus_total":4,"item_index":0,"item_total":2,
                "lane":"top_2","title":"FLOCK CAM","detail":"B4:1E:52 -57dB",
                "evidence":"oui b4:1e:52","entity_key":"flock:b4:1e:52",
                "display_id":"B4:1E:52","class":"flock","category":"FLOCK",
                "code":"FLK","source":"wifi_oui","score":92,"rssi":-57
              },
              "ble_control":{
                "enabled":true,"bonded":true,"pairing_age_s":9,
                "pairing_window_s":10,"connected":false,"encrypted":true,
                "last_error":"not connected","rx":17,"tx":22
              },
              "entities":[{
                "label":"FLOCK CAM","detail":"camera oui b4:1e:52",
                "evidence":"wifi oui match","class":"flock","category":"FLOCK",
                "code":"FLK","display_id":"B4:1E:52","source":"wifi_oui",
                "source_id":7,"score":92,"confidence_pct":88,"evidence_quality":5,
                "display_rank":1000,"age_s":4,"last_seen_s":1,"rssi":-57,
                "best_rssi":-55,"events":3,"seen_count":4,"group_count":1,
                "proximity_level":3,"stale":false
              }],
              "scanners":[{
                "slot":0,"uart":"ble","connected":true,"slot_role":"ble_primary",
                "expected_scan_profile":"ble_primary","scan_profile":"ble_primary",
                "role_acked":true,"health":"ok","display_policy_hash":$policyHash,
                "display_policy_ack_hash":$policyHash,"filtered_counts":{"beacon":2}
              }],
              "safe_mode":true,"safe_reason":"crash_loop","reset_reason":"PANIC",
              "crash_count":2,"recovery_mode":"safe_usb",
              "stack_main_free":4096,"stack_display_free":3072,"stack_usb_free":2048,
              "stack_uart_ble_free":6144,"stack_uart_wifi_free":7168,
              "heap_internal_free":123456,"heap_internal_min_free":65432,
              "psram_free":7340032,
              "debug_bridge":{
                "serial_port":"/dev/cu.usbmodem1101","status_age_s":1.25,"last_error":""
              }
            }
        """.trimIndent()
    }

    companion object {
        private val completeThemeJson = """
            {
              "version":1,"palette":"field","background":"dark","brightness":100,
              "accents":{
                "drone":65184,"meta":63539,"tracker":63519,
                "flock":43039,"wifi_attack":2047,"clear":12133
              }
            }
        """.trimIndent()

        private val completePolicyJson = """
            {
              "version":1,
              "classes":{
                "drone":{"enabled":true,"lane":"both","min_proximity":"present","priority":100},
                "meta":{"enabled":true,"lane":"both","min_proximity":"present","priority":95},
                "tracker":{"enabled":true,"lane":"lower","min_proximity":"near","priority":70},
                "wifi_attack":{"enabled":true,"lane":"both","min_proximity":"present","priority":90},
                "skimmer":{"enabled":true,"lane":"both","min_proximity":"near","priority":88},
                "camera":{"enabled":true,"lane":"lower","min_proximity":"near","priority":65},
                "flock":{"enabled":true,"lane":"both","min_proximity":"present","priority":85},
                "lock":{"enabled":true,"lane":"lower","min_proximity":"near","priority":55},
                "hid":{"enabled":true,"lane":"lower","min_proximity":"close","priority":45},
                "beacon":{"enabled":true,"lane":"lower","min_proximity":"near","priority":30},
                "event_badge":{"enabled":true,"lane":"lower","min_proximity":"near","priority":35},
                "auracast":{"enabled":true,"lane":"lower","min_proximity":"near","priority":20},
                "scanner_status":{"enabled":true,"lane":"lower","min_proximity":"present","priority":10}
              }
            }
        """.trimIndent()
    }
}
