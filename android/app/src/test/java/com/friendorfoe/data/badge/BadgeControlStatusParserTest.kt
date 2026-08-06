package com.friendorfoe.data.badge

import com.google.gson.JsonParser
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlStatusParserTest {

    @Test
    fun `parses the frozen firmware USB health fixture and immutable runtime fields`() {
        val fixtureFile = generateSequence(File(".").canonicalFile) { it.parentFile }
            .map { root -> File(root, "docs/badge/protocol/badge_usb_health_v1.fixture.json") }
            .firstOrNull(File::isFile)
            ?: error("Unable to locate badge USB health fixture")
        val fixtureJson = fixtureFile.readText()
        val usbHealthObject = JsonParser.parseString(fixtureJson)
            .asJsonObject
            .getAsJsonObject("usb_health")

        assertEquals(
            setOf(
                "schema",
                "task_started",
                "host_connected",
                "parser_state",
                "rx_bytes",
                "valid_commands",
                "responses_completed",
                "required_response_failures",
                "malformed_lines",
                "dropped_progress_frames",
                "dropped_optional_frames",
                "upload_received",
                "upload_size",
                "task_heartbeat_age_s",
                "last_rx_age_s",
                "last_command_age_s",
                "last_response_age_s",
                "last_upload_progress_age_s",
            ),
            usbHealthObject.keySet(),
        )

        val root = JsonParser.parseString(fixtureJson).asJsonObject.apply {
            addProperty("hardware_id", "A4:CF:12:34:56:78")
            addProperty("running_partition", "ota_1")
            addProperty("pending_verify", true)
            addProperty("rollback_state", "pending_verify")
            addProperty("last_expected_reboot_reason", "android_control_reboot")
        }
        val status = parseBadgeControlStatus(root.toString())

        assertNotNull(status)
        status!!
        assertEquals("A4:CF:12:34:56:78", status.hardwareId)
        assertEquals("ota_1", status.runningPartition)
        assertTrue(status.pendingVerify)
        assertEquals("pending_verify", status.rollbackState)
        assertEquals("android_control_reboot", status.lastExpectedRebootReason)
        assertEquals(
            BadgeUsbHealthStatus(
                schema = 1,
                taskStarted = true,
                hostConnected = true,
                parserState = "command",
                rxBytes = 128L,
                validCommands = 3L,
                responsesCompleted = 3L,
                requiredResponseFailures = 0L,
                malformedLines = 0L,
                droppedProgressFrames = 0L,
                droppedOptionalFrames = 0L,
                uploadReceived = 0L,
                uploadSize = 0L,
                taskHeartbeatAgeSeconds = 0L,
                lastRxAgeSeconds = 0L,
                lastCommandAgeSeconds = 0L,
                lastResponseAgeSeconds = 0L,
                lastUploadProgressAgeSeconds = null,
            ),
            status.usbHealth,
        )
    }

    @Test
    fun `legacy and independently malformed runtime health fields fail closed to defaults`() {
        val legacy = parseBadgeControlStatus("{\"version\":\"legacy\"}")
        assertNotNull(legacy)
        legacy!!
        assertEquals("", legacy.hardwareId)
        assertEquals("", legacy.runningPartition)
        assertFalse(legacy.pendingVerify)
        assertEquals("", legacy.rollbackState)
        assertEquals("", legacy.lastExpectedRebootReason)
        assertEquals(BadgeUsbHealthStatus(), legacy.usbHealth)

        val malformed = parseBadgeControlStatus(
            """
            {
              "hardware_id":17,
              "running_partition":false,
              "pending_verify":"true",
              "rollback_state":{},
              "last_expected_reboot_reason":[],
              "usb_health":{
                "schema":"1",
                "task_started":"true",
                "host_connected":1,
                "parser_state":99,
                "rx_bytes":"128",
                "valid_commands":{},
                "responses_completed":[],
                "required_response_failures":false,
                "malformed_lines":null,
                "dropped_progress_frames":"0",
                "dropped_optional_frames":true,
                "upload_received":{},
                "upload_size":[],
                "task_heartbeat_age_s":"0",
                "last_rx_age_s":false,
                "last_command_age_s":{},
                "last_response_age_s":[],
                "last_upload_progress_age_s":"never"
              }
            }
            """.trimIndent(),
        )

        assertNotNull(malformed)
        malformed!!
        assertEquals("", malformed.hardwareId)
        assertEquals("", malformed.runningPartition)
        assertFalse(malformed.pendingVerify)
        assertEquals("", malformed.rollbackState)
        assertEquals("", malformed.lastExpectedRebootReason)
        assertEquals(BadgeUsbHealthStatus(), malformed.usbHealth)

        val independentlyDefaulted = parseBadgeControlStatus(
            """
            {
              "hardware_id":"A4:CF:12:34:56:78",
              "running_partition":{},
              "pending_verify":true,
              "rollback_state":"clear",
              "last_expected_reboot_reason":false,
              "usb_health":{
                "schema":1,
                "task_started":"bad",
                "host_connected":true,
                "parser_state":"line",
                "rx_bytes":7,
                "valid_commands":"bad",
                "responses_completed":5,
                "last_rx_age_s":2,
                "last_command_age_s":{}
              }
            }
            """.trimIndent(),
        )

        assertNotNull(independentlyDefaulted)
        independentlyDefaulted!!
        assertEquals("A4:CF:12:34:56:78", independentlyDefaulted.hardwareId)
        assertEquals("", independentlyDefaulted.runningPartition)
        assertTrue(independentlyDefaulted.pendingVerify)
        assertEquals("clear", independentlyDefaulted.rollbackState)
        assertEquals("", independentlyDefaulted.lastExpectedRebootReason)
        assertEquals(1, independentlyDefaulted.usbHealth.schema)
        assertFalse(independentlyDefaulted.usbHealth.taskStarted)
        assertTrue(independentlyDefaulted.usbHealth.hostConnected)
        assertEquals("", independentlyDefaulted.usbHealth.parserState)
        assertEquals(7L, independentlyDefaulted.usbHealth.rxBytes)
        assertEquals(0L, independentlyDefaulted.usbHealth.validCommands)
        assertEquals(5L, independentlyDefaulted.usbHealth.responsesCompleted)
        assertEquals(2L, independentlyDefaulted.usbHealth.lastRxAgeSeconds)
        assertEquals(null, independentlyDefaulted.usbHealth.lastCommandAgeSeconds)
    }

    @Test
    fun `USB health accepts only firmware parser enums and non negative numeric fields`() {
        listOf("command", "scanner_upload", "uplink_upload").forEach { parserState ->
            val status = parseBadgeControlStatus(
                """{"usb_health":{"parser_state":"$parserState"}}""",
            )
            assertNotNull(status)
            assertEquals(parserState, status!!.usbHealth.parserState)
        }

        val invalid = parseBadgeControlStatus(
            """
            {
              "usb_health":{
                "schema":-1,
                "parser_state":"line",
                "rx_bytes":-1,
                "valid_commands":-2,
                "responses_completed":-3,
                "required_response_failures":-4,
                "malformed_lines":-5,
                "dropped_progress_frames":-6,
                "dropped_optional_frames":-7,
                "upload_received":-8,
                "upload_size":-9,
                "task_heartbeat_age_s":-10,
                "last_rx_age_s":-11,
                "last_command_age_s":-12,
                "last_response_age_s":-13,
                "last_upload_progress_age_s":-14
              }
            }
            """.trimIndent(),
        )

        assertNotNull(invalid)
        assertEquals(BadgeUsbHealthStatus(), invalid!!.usbHealth)
    }

    @Test
    fun `parses complete six accent readback for every wire palette`() {
        val fixtures = listOf(
            ThemeReadbackFixture(
                palette = "field",
                background = "dark",
                brightness = 100,
                accents = listOf(0xFEA0, 0xF833, 0xF81F, 0xA81F, 0x07FF, 0x2F65),
            ),
            ThemeReadbackFixture(
                palette = "night",
                background = "dim",
                brightness = 90,
                accents = listOf(0xFD83, 0xF9AB, 0xFA44, 0xC349, 0x3EFE, 0x7FEE),
            ),
            ThemeReadbackFixture(
                palette = "neon",
                background = "scanline",
                brightness = 75,
                accents = listOf(0xCFE5, 0xFA75, 0x99DA, 0xA357, 0x373F, 0xBFE9),
            ),
            ThemeReadbackFixture(
                palette = "mono",
                background = "scanline",
                brightness = 50,
                accents = listOf(0xD7EA, 0x57B5, 0x26AF, 0x554F, 0x37FB, 0xAFEC),
            ),
        )

        fixtures.forEach { fixture ->
            val status = parseBadgeControlStatus(fixture.statusJson())

            assertNotNull("Expected ${fixture.palette} theme status", status)
            val theme = status!!.theme
            assertEquals(1, theme.version)
            assertEquals(fixture.palette, theme.palette)
            assertEquals(fixture.background, theme.background)
            assertEquals(fixture.brightness, theme.brightness)
            assertEquals(
                BadgeThemeAccentClasses.map { it.key },
                theme.accents.keys.toList(),
            )
            assertEquals(
                fixture.accents,
                BadgeThemeAccentClasses.map { theme.accents.getValue(it.key) },
            )
        }
    }

    @Test
    fun `partial legacy theme readback keeps version one defaults`() {
        val status = parseBadgeControlStatus(
            """
            {
              "theme":{
                "palette":"mono",
                "accents":{"tracker":4660}
              }
            }
            """.trimIndent(),
        )

        assertNotNull(status)
        val theme = status!!.theme
        assertEquals(1, theme.version)
        assertEquals("mono", theme.palette)
        assertEquals("dark", theme.background)
        assertEquals(100, theme.brightness)
        assertEquals(
            listOf(0xFEA0, 0xF833, 0x1234, 0xA81F, 0x07FF, 0x2F65),
            BadgeThemeAccentClasses.map { theme.accents.getValue(it.key) },
        )
    }

    @Test
    fun `complete arbitrary custom theme readback is not recognized as a preset`() {
        val customAccents = listOf(0x0841, 0x18C3, 0x2945, 0x39C7, 0x4A49, 0x5ACB)
        val status = parseBadgeControlStatus(
            ThemeReadbackFixture(
                palette = "night",
                background = "dim",
                brightness = 65,
                accents = customAccents,
            ).statusJson(),
        )

        assertNotNull(status)
        val theme = status!!.theme
        assertEquals("night", theme.palette)
        assertEquals("dim", theme.background)
        assertEquals(65, theme.brightness)
        assertEquals(
            customAccents,
            BadgeThemeAccentClasses.map { theme.accents.getValue(it.key) },
        )
        assertEquals(null, recognizeBadgeThemePreset(theme))
    }

    @Test
    fun parsesExtendedBadgeStatusPayload() {
        val status = parseBadgeControlStatus(
            """
            {
              "version":"0.64.40-badge-ble-theme",
              "mode":"usb_only",
              "mode_label":"USB Only",
              "threat_score":77.5,
              "color_rgb565":63488,
              "counts":{"drone":2,"meta":1,"tracker":1,"wifi_anomaly":1,"ble":3,"other":4},
              "display_policy_hash":123456,
              "display_policy":{
                "version":1,
                "classes":{
                  "drone":{"enabled":true,"lane":"both","min_proximity":"near","priority":99},
                  "beacon":{"enabled":false,"lane":"off","min_proximity":"close","priority":5}
                }
              },
              "filtered_counts":{"beacon":12,"scanner_status":3},
              "theme_hash":98765,
              "theme":{
                "version":1,
                "palette":"night",
                "background":"scanline",
                "brightness":80,
                "accents":{"meta":63488,"flock":2016}
              },
              "display_state":{
                "active":true,
                "detail_mode":true,
                "detail_page":2,
                "focus_index":1,
                "focus_total":4,
                "item_index":0,
                "item_total":2,
                "lane":"top_2",
                "title":"FLOCK CAM",
                "detail":"B4:1E:52 -57dB",
                "evidence":"oui b4:1e:52",
                "entity_key":"flock:b4:1e:52",
                "display_id":"B4:1E:52",
                "class":"flock",
                "category":"FLOCK",
                "code":"FLK",
                "source":"wifi_oui",
                "score":92,
                "confidence_pct":88,
                "evidence_quality":5,
                "display_rank":1000,
                "age_s":4,
                "last_seen_s":1,
                "rssi":-57,
                "best_rssi":-55,
                "events":3,
                "seen_count":4,
                "group_count":1,
                "proximity_level":3,
                "stale":false,
                "lat":36.1001,
                "lon":-115.2002,
                "altitude_m":620.5,
                "operator_lat":36.2001,
                "operator_lon":-115.3002,
                "operator_id":"OP-7"
              },
              "ble_control":{
                "enabled":true,
                "bonded":true,
                "pairing_age_s":9,
                "pairing_window_s":10,
                "connected":false,
                "encrypted":true,
                "last_error":"not connected",
                "rx":17,
                "tx":22
              },
              "entities":[{
                "label":"FLOCK CAM",
                "detail":"camera oui b4:1e:52",
                "evidence":"wifi oui match",
                "class":"flock",
                "category":"FLOCK",
                "code":"FLK",
                "display_id":"B4:1E:52",
                "source":"wifi_oui",
                "source_id":7,
                "score":92,
                "confidence_pct":88,
                "evidence_quality":5,
                "display_rank":1000,
                "age_s":4,
                "last_seen_s":1,
                "rssi":-57,
                "best_rssi":-55,
                "events":3,
                "seen_count":4,
                "group_count":1,
                "proximity_level":3,
                "stale":false,
                "lat":36.1001,
                "lon":-115.2002,
                "altitude_m":620.5,
                "operator_lat":36.2001,
                "operator_lon":-115.3002,
                "operator_id":"OP-7"
              }],
              "scanners":[{
                "slot":0,
                "uart":"ble",
                "connected":true,
                "slot_role":"ble_primary",
                "expected_scan_profile":"ble_primary",
                "scan_profile":"ble_primary",
                "role_acked":true,
                "health":"ok",
                "uart_raw_seen":true,
                "uart_raw_age_s":2,
                "uart_json_err":1,
                "cmd_rx":22,
                "cmd_last_age_s":3,
                "ble_adv_seen":100,
                "ble_fp_emit":5,
                "ble_meta_seen":1,
                "ble_tracker_seen":2,
                "rid_emit":1,
                "privacy_seen":7,
                "wifi_total_frames":200,
                "wifi_drone_ssid_emit":1,
                "wifi_notable_ssid_emit":2,
                "wifi_last_drone_ssid":"DroneNet",
                "wifi_last_notable_ssid":"flock",
                "display_policy_hash":123456,
                "display_policy_ack_hash":123456,
                "filtered_counts":{"beacon":2},
                "fw_state":"idle",
                "target_ver":"0.64.39",
                "ota_state":"ok",
                "last_fw_error":""
              }],
              "safe_mode":true,
              "safe_reason":"crash_loop",
              "reset_reason":"PANIC",
              "reset_reason_code":4,
              "reset_expected":false,
              "crash_count":2,
              "recovery_mode":"safe_usb",
              "usb_control_age_s":1,
              "stack_main_free":4096,
              "stack_display_free":3072,
              "stack_usb_free":2048,
              "stack_uart_ble_free":6144,
              "stack_uart_wifi_free":7168,
              "heap_internal_free":123456,
              "heap_internal_min_free":65432,
              "heap_internal_largest":32768,
              "psram_total":8388608,
              "psram_free":7340032,
              "psram_largest":4194304
            }
            """.trimIndent()
        )

        assertNotNull(status)
        status!!
        assertEquals("0.64.40-badge-ble-theme", status.version)
        assertEquals("USB Only", status.modeLabel)
        assertEquals(2, status.counts.drone)
        assertEquals(1, status.counts.meta)
        assertEquals(123456L, status.displayPolicyHash)
        assertEquals("near", status.displayPolicy.classes.getValue("drone").minProximity)
        assertFalse(status.displayPolicy.classes.getValue("beacon").enabled)
        assertEquals(12, status.filteredCounts.getValue("beacon"))
        assertEquals(98765L, status.themeHash)
        assertEquals("night", status.theme.palette)
        assertEquals("scanline", status.theme.background)
        assertEquals(80, status.theme.brightness)
        assertEquals(63488, status.theme.accents.getValue("meta"))
        assertTrue(status.safeMode)
        assertEquals("PANIC", status.resetReason)
        assertEquals(2, status.crashCount)
        assertEquals(6144, status.stackUartBleFree)
        assertEquals(7340032L, status.psramFree)

        val display = status.displayState
        assertNotNull(display)
        display!!
        assertTrue(display.detailMode)
        assertEquals("FLOCK CAM", display.title)
        assertEquals("wifi_oui", display.source)
        assertEquals(-57, display.rssi)
        assertEquals(36.1001, display.lat!!, 0.00001)
        assertEquals("OP-7", display.operatorId)
        assertTrue(status.bleControl.enabled)
        assertTrue(status.bleControl.bonded)
        assertEquals(9L, status.bleControl.pairingAgeSeconds)
        assertEquals(10, status.bleControl.pairingWindowSeconds)
        assertFalse(status.bleControl.connected)
        assertTrue(status.bleControl.encrypted)
        assertEquals("not connected", status.bleControl.lastError)
        assertEquals(17L, status.bleControl.rx)
        assertEquals(22L, status.bleControl.tx)

        val entity = status.entities.single()
        assertEquals("FLOCK CAM", entity.label)
        assertEquals("wifi oui match", entity.evidence)
        assertEquals("B4:1E:52", entity.displayId)
        assertEquals(88, entity.confidencePct)
        assertEquals(-55, entity.bestRssi)
        assertEquals(36.2001, entity.operatorLat!!, 0.00001)

        val scanner = status.scanners.single()
        assertEquals("ble", scanner.uart)
        assertEquals("ble_primary", scanner.scanProfile)
        assertTrue(scanner.roleAcked)
        assertEquals(123456L, scanner.displayPolicyAckHash)
        assertEquals(2, scanner.filteredCounts.getValue("beacon"))
        assertEquals("DroneNet", scanner.wifiLastDroneSsid)
    }

    @Test
    fun parsesBadgeEvilTwinEntityEvidence() {
        val status = parseBadgeControlStatus(
            """
            {
              "version":"0.64.40-badge-ble-theme",
              "mode":"usb_only",
              "counts":{"wifi_anomaly":1},
              "entities":[{
                "label":"Evil Twin",
                "detail":"ssid CafeWiFi",
                "evidence":"Evil Twin: open clone vs WPA2; ref 00:11:22:33:44:55 ch6",
                "class":"wifi_anomaly",
                "category":"WIFI",
                "code":"WIFI",
                "display_id":"66:77:88:99:AA:BB",
                "source":"wifi_assoc",
                "source_id":7,
                "ssid":"CafeWiFi",
                "bssid":"66:77:88:99:AA:BB",
                "auth_m":0,
                "freq_mhz":2437,
                "score":88,
                "confidence_pct":82,
                "evidence_quality":6,
                "display_rank":30642,
                "age_s":3,
                "last_seen_s":1,
                "rssi":-48,
                "best_rssi":-48,
                "events":1,
                "seen_count":1,
                "group_count":1,
                "proximity_level":3,
                "stale":false
              }]
            }
            """.trimIndent()
        )

        assertNotNull(status)
        status!!
        val entity = status.entities.single()
        assertEquals("Evil Twin", entity.label)
        assertEquals("ssid CafeWiFi", entity.detail)
        assertEquals("66:77:88:99:AA:BB", entity.displayId)
        assertEquals("CafeWiFi", entity.ssid)
        assertEquals("66:77:88:99:AA:BB", entity.bssid)
        assertEquals(0, entity.authMode)
        assertEquals(2437, entity.freqMhz)
        assertEquals("wifi_assoc", entity.source)
        assertEquals(-48, entity.rssi)
        assertTrue(entity.evidence.contains("open clone"))
    }

    @Test
    fun parsesDroneDisplayClassDisabledFromBadgeStatus() {
        val status = parseBadgeControlStatus(
            """
            {
              "mode":"usb_only",
              "display_policy":{
                "version":1,
                "classes":{
                  "drone":{"enabled":false,"lane":"off","min_proximity":"present","priority":100}
                }
              },
              "filtered_counts":{"drone":4}
            }
            """.trimIndent()
        )

        assertNotNull(status)
        status!!
        val dronePolicy = status.displayPolicy.classes.getValue("drone")
        assertFalse(dronePolicy.enabled)
        assertEquals("off", dronePolicy.lane)
        assertEquals(4, status.filteredCounts.getValue("drone"))
    }

    @Test
    fun `parses Lite protocol identity capabilities and scanner summaries`() {
        val status = parseBadgeControlStatus(
            """
            {
              "version":"0.2.0-backend",
              "product_family":"badge_lite",
              "target":"uplink-s3-backend",
              "project":"fof_backend_uplink",
              "hardware":"seeed_xiao_esp32s3",
              "mode":"headless",
              "capabilities":["display_none","usb_live","usb_live_ack","usb_config"],
              "scanner_summaries":[{"slot":0,"connected":true,"health":"ready"}]
            }
            """.trimIndent(),
        )

        assertNotNull(status)
        status!!
        assertEquals(BADGE_LITE_PRODUCT_ID, status.productFamily)
        assertEquals(BADGE_LITE_PROJECT, status.protocolProject)
        assertEquals(BADGE_LITE_HARDWARE, status.protocolHardware)
        assertEquals(
            setOf("display_none", "usb_live", "usb_live_ack", "usb_config"),
            status.capabilities,
        )
        assertTrue(status.capabilitiesWellFormed)
        assertEquals(1, status.scanners.size)
        assertTrue(status.scanners.single().connected)

        val malformedCapabilities = parseBadgeControlStatus(
            """{"capabilities":["usb_live","usb_live"]}""",
        )
        assertNotNull(malformedCapabilities)
        assertFalse(malformedCapabilities!!.capabilitiesWellFormed)
    }

    private data class ThemeReadbackFixture(
        val palette: String,
        val background: String,
        val brightness: Int,
        val accents: List<Int>,
    ) {
        init {
            require(accents.size == BadgeThemeAccentClasses.size)
        }

        fun statusJson(): String {
            val encodedAccents = BadgeThemeAccentClasses.mapIndexed { index, accent ->
                "\"${accent.key}\":${accents[index]}"
            }.joinToString(",")
            return """
                {
                  "theme":{
                    "version":1,
                    "palette":"$palette",
                    "background":"$background",
                    "brightness":$brightness,
                    "accents":{$encodedAccents}
                  }
                }
            """.trimIndent()
        }
    }
}
