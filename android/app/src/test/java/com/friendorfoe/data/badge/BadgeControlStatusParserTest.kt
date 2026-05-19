package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlStatusParserTest {

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
}
