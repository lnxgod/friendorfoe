package com.friendorfoe.data.remote

import com.google.gson.Gson
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SensorMapApiServiceDtoTest {

    private val gson = Gson()

    @Test
    fun parses_node_status_with_scanner_diagnostics() {
        val json = """
            {
              "count": 1,
              "nodes": [
                {
                  "device_id": "gate",
                  "online": true,
                  "age_s": 1.2,
                  "firmware_version": "0.63.0-svc132",
                  "board_type": "uplink-s3",
                  "scan_mode": "calibration",
                  "scan_profile": "calibration",
                  "dedup_seen": 20,
                  "dedup_sent": 12,
                  "dedup_collapsed": 8,
                  "cal_seen": 10,
                  "cal_sent": 6,
                  "source_fixups_recent": 2,
                  "scanners": [
                    {
                      "uart": "ble",
                      "ver": "0.63.0-svc132",
                      "board": "scanner-s3-combo-seed",
                      "scan_profile": "calibration",
                      "slot_role": "ble_primary",
                      "tx_queue_depth": 3,
                      "tx_queue_capacity": 64,
                      "tx_queue_pressure_pct": 4,
                      "uart_tx_dropped": 1,
                      "probe_drop_pressure": 2
                    }
                  ]
                }
              ]
            }
        """.trimIndent()

        val dto = gson.fromJson(json, NodeStatusDto::class.java)

        assertEquals(1, dto.count)
        assertEquals("gate", dto.nodes.first().deviceId)
        assertEquals("scanner-s3-combo-seed", dto.nodes.first().scanners.first().board)
        assertEquals(2, dto.nodes.first().sourceFixupsRecent)
        assertEquals("calibration", dto.nodes.first().scanMode)
        assertEquals(8, dto.nodes.first().dedupCollapsed)
        assertEquals("ble_primary", dto.nodes.first().scanners.first().slotRole)
    }

    @Test
    fun parses_map_geometry_trust_fields() {
        val json = """
            {
              "drone_count": 1,
              "sensor_count": 2,
              "drones": [
                {
                  "drone_id": "PROBE:A1B2C3D4",
                  "lat": 30.1,
                  "lon": -97.7,
                  "position_source": "intersection",
                  "sensor_count": 2,
                  "confidence": 0.4,
                  "identity_source": "probe_fingerprint",
                  "range_authority": "backend_rssi",
                  "geometry_trust": "diagnostic",
                  "source_tier": "diagnostic",
                  "uncertainty_m": 35.5,
                  "calibration_state": "defaults",
                  "observations": [
                    {
                      "device_id": "node-a",
                      "sensor_lat": 30.0,
                      "sensor_lon": -97.0,
                      "distance_source": "backend_rssi",
                      "range_authority": "backend_rssi",
                      "geometry_trust": "diagnostic",
                      "source_tier": "diagnostic",
                      "uncertainty_m": 30.0,
                      "source": "wifi_probe_request"
                    }
                  ]
                }
              ],
              "sensors": []
            }
        """.trimIndent()

        val dto = gson.fromJson(json, DroneMapDto::class.java)
        val drone = dto.drones.first()
        val obs = drone.observations.first()

        assertEquals("probe_fingerprint", drone.identitySource)
        assertEquals("diagnostic", drone.geometryTrust)
        assertEquals(35.5, drone.uncertaintyM!!, 0.001)
        assertEquals("backend_rssi", obs.rangeAuthority)
        assertEquals("diagnostic", obs.sourceTier)
    }

    @Test
    fun parses_probe_device_and_event_stats_payloads() {
        val probesJson = """
            {
              "count": 1,
              "devices": [
                {
                  "identity": "PROBE:A1B2C3D4",
                  "ie_hash": "A1B2C3D4",
                  "macs": ["AA:AA:AA:AA:AA:01"],
                  "probed_ssids": ["DJI-1234"],
                  "first_seen": 1713800000.0,
                  "first_seen_age_s": 7200.0,
                  "last_seen": 1713807100.0,
                  "age_s": 300.0,
                  "seen_24h_count": 17,
                  "sensor_count_24h": 3,
                  "activity_level": "high",
                  "latest_event_types": ["new_probe_identity", "probe_activity_spike"]
                }
              ]
            }
        """.trimIndent()
        val statsJson = """
            {
              "total": 5,
              "unacknowledged": 4,
              "critical_unacked": 0,
              "by_type": {"new_probe_identity": 2},
              "unack_by_type": {"new_probe_identity": 2, "new_probed_ssid": 1},
              "by_severity": {"info": 3, "warning": 1}
            }
        """.trimIndent()

        val probes = gson.fromJson(probesJson, ProbeDevicesDto::class.java)
        val stats = gson.fromJson(statsJson, EventStatsDto::class.java)

        assertEquals("PROBE:A1B2C3D4", probes.devices.first().identity)
        assertEquals("high", probes.devices.first().activityLevel)
        assertTrue(probes.devices.first().latestEventTypes.contains("probe_activity_spike"))
        assertEquals(2, stats.unackByType["new_probe_identity"])
        assertEquals(1, stats.unackByType["new_probed_ssid"])
    }
}
