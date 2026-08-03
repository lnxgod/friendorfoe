package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeLiteUsbCompatibilityTest {

    @Test
    fun `truthful Badge Lite identity accepts each documented alias set`() {
        val backendNames = parseBadgeControlStatus(
            """{
                "product_family":"badge_lite",
                "target":"uplink-s3-backend",
                "project":"fof_backend_uplink",
                "hardware":"seeed_xiao_esp32s3",
                "mac":"a4:cf:12:34:56:78",
                "version":"0.2.0-backend",
                "mode":"headless",
                "capabilities":["display_none","usb_live","usb_live_ack"]
            }""".trimIndent(),
        )
        val badgeAliases = parseBadgeControlStatus(
            """{
                "product_family":"badge_lite",
                "target":"uplink-s3-backend",
                "app_project":"fof_backend_uplink",
                "hardware_type":"seeed_xiao_esp32s3",
                "hardware_id":"A4:CF:12:34:56:78",
                "version":"0.2.0-backend",
                "mode":"headless",
                "capabilities":["display_none"]
            }""".trimIndent(),
        )

        listOf(backendNames, badgeAliases).forEach { status ->
            assertNotNull(status)
            assertNull(badgeUsbIdentityError(status))
            assertEquals(BadgeUsbProductKind.BADGE_LITE, badgeUsbProductKind(status))
            assertEquals(BadgeUsbStatus.CONNECTED, badgeUsbHandshakeStatus(status))
        }
        assertNull(
            badgeUsbIdentityError(
                liteStatus().copy(
                    mac = "a4:cf:12:34:56:78",
                    hardwareId = "A4:CF:12:34:56:78",
                ),
            ),
        )

        val owner = badgeUsbOwnerKeyFromHandshake(
            status = backendNames,
            attachmentToken = attachmentToken(),
            lifecycleSession = 7L,
            connectionIdentity = Any(),
            endpointIdentity = Any(),
        )
        assertNotNull(owner)
        assertEquals(BadgeUsbProductKind.BADGE_LITE, owner?.productKind)
        assertEquals("A4:CF:12:34:56:78", owner?.hardwareId)
    }

    @Test
    fun `Badge Lite identity is exact and conflicting aliases fail closed`() {
        val invalid = listOf(
            liteStatus().copy(productFamily = "esp32"),
            liteStatus().copy(firmwareTarget = "uplink-s3-backend-extra"),
            liteStatus().copy(firmwareName = "uplink-s3-fof_badge"),
            liteStatus().copy(project = "other"),
            liteStatus().copy(hardware = "generic_esp32s3"),
            liteStatus().copy(mac = "00:00:00:00:00:00"),
            liteStatus().copy(mode = "local_ap"),
            liteStatus().copy(capabilities = setOf("usb_live")),
            liteStatus().copy(
                project = "fof_backend_uplink",
                appProject = "conflicting_project",
            ),
            liteStatus().copy(
                hardware = "seeed_xiao_esp32s3",
                hardwareType = "esp32s3",
            ),
            liteStatus().copy(
                mac = "A4:CF:12:34:56:78",
                hardwareId = "A4:CF:12:34:56:79",
            ),
        )

        invalid.forEach { status ->
            assertNotNull(status.toString(), badgeUsbIdentityError(status))
            assertEquals(BadgeUsbStatus.ERROR, badgeUsbHandshakeStatus(status))
        }
    }

    @Test
    fun `native badge tuple remains exact and never opts into Lite live frames`() {
        val nativeStatus = BadgeControlStatus(
            version = "0.64.76-badge-defcon34",
            firmwareTarget = "uplink-s3-fof_badge",
            firmwareName = "uplink-s3-fof_badge",
            appProject = "fof_badge_uplink",
            hardwareType = "seeed_xiao_esp32s3",
            hardwareId = "A4:CF:12:34:56:78",
        )
        assertNull(badgeUsbIdentityError(nativeStatus))
        assertEquals(BadgeUsbProductKind.NATIVE_BADGE, badgeUsbProductKind(nativeStatus))

        val nativeOwner = owner(BadgeUsbProductKind.NATIVE_BADGE)
        val gate = BadgeUsbLiteLiveGate()
        assertNull(badgeUsbLiteLiveStartLine(nativeOwner))
        assertFalse(gate.bind(nativeOwner))

        listOf(
            nativeStatus.copy(firmwareName = "scanner-s3-combo-fof_badge"),
            nativeStatus.copy(appProject = "fof_badge_scanner"),
            nativeStatus.copy(hardwareType = "esp32s3"),
            nativeStatus.copy(hardwareId = "not-a-mac"),
        ).forEach { status ->
            assertNotNull(badgeUsbIdentityError(status))
        }
    }

    @Test
    fun `Lite live session requires verified owner READY and monotonic heartbeat ACKs`() {
        val liteOwner = owner(BadgeUsbProductKind.BADGE_LITE)
        val staleOwner = liteOwner.copy(endpointIdentity = Any())
        val gate = BadgeUsbLiteLiveGate()
        val ready = parseBadgeUsbLiteLiveReady(
            "FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":5000,\"lease_ms\":15000}",
        )
        val heartbeat = parseBadgeUsbLiteLiveHeartbeat(
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":7}",
        )
        assertNotNull(ready)
        assertNotNull(heartbeat)
        assertEquals(BADGE_LITE_LIVE_START_LINE, badgeUsbLiteLiveStartLine(liteOwner))
        assertTrue(gate.bind(liteOwner))
        assertNull(gate.prepareAck(liteOwner, heartbeat!!))
        assertFalse(gate.acceptReady(staleOwner, ready!!))
        assertTrue(gate.acceptReady(liteOwner, ready))
        assertEquals("boot-a1", gate.activeSession(liteOwner))

        val ticket = gate.prepareAck(liteOwner, heartbeat)
        assertNotNull(ticket)
        assertEquals(
            "FOF_LIVE_ACK:{\"session_id\":\"boot-a1\",\"sequence\":7}",
            badgeUsbLiteLiveAckLine(ticket!!.sessionId, ticket.sequence),
        )
        assertTrue(gate.completeAck(ticket, sent = true))
        assertNull(gate.prepareAck(liteOwner, heartbeat))

        val next = parseBadgeUsbLiteLiveHeartbeat(
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":8}",
        )!!
        assertNotNull(gate.prepareAck(liteOwner, next))
    }

    @Test
    fun `Lite live frame parsing rejects malformed sessions extensions and sequence values`() {
        listOf(
            "FOF_LIVE_READY:{\"session_id\":\"boot\",\"heartbeat_ms\":5000}",
            "FOF_LIVE_READY:{\"session_id\":\"boot\",\"heartbeat_ms\":5000,\"lease_ms\":1000}",
            "FOF_LIVE_READY:{\"session_id\":\"boot\",\"heartbeat_ms\":5000,\"lease_ms\":15000,\"extra\":1}",
            "FOF_LIVE_READY:{\"session_id\":17,\"heartbeat_ms\":5000,\"lease_ms\":15000}",
        ).forEach { assertNull(it, parseBadgeUsbLiteLiveReady(it)) }

        listOf(
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot\",\"sequence\":0}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot\",\"sequence\":-1}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot\",\"sequence\":\"1\"}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot\",\"sequence\":18446744073709551616}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot\",\"sequence\":1,\"extra\":true}",
        ).forEach { assertNull(it, parseBadgeUsbLiteLiveHeartbeat(it)) }
    }

    @Test
    fun `Lite scanner aliases and FOF DET payload preserve Android detection behavior`() {
        val status = parseBadgeControlStatus(
            """{
                "scanner":[
                    {"slot":0,"connected":true,"identity_valid":true,"profile":2,
                     "health":{"command":true,"radio":true,"role_acked":true}},
                    {"slot":1,"connected":false,"identity_valid":false}
                ]
            }""".trimIndent(),
        )
        assertEquals(2, status?.scanners?.size)
        assertEquals("2", status?.scanners?.first()?.scanProfile)
        assertEquals("healthy", status?.scanners?.first()?.health)
        assertTrue(status?.scanners?.first()?.roleAcked == true)
        assertEquals("disconnected", status?.scanners?.last()?.health)

        val detection = parseBadgeUsbDetection(
            """{
                "id":"rid-lite-7","manufacturer":"DJI","badge_label":"Drone",
                "badge_class":"drone","badge_entity_key":"drone:lite-7",
                "source":0,"confidence":0.875,"threat_score":72,"rssi":-45
            }""".trimIndent(),
            receivedAtElapsedMs = 1234L,
        )
        assertEquals("rid-lite-7", detection?.id)
        assertEquals("drone:lite-7", detection?.badgeEntityKey)
        assertEquals(72f, detection?.threatScore)
        assertEquals(1234L, detection?.receivedAtElapsedMs)
    }

    @Test
    fun `headless display-none badges gate every display control but retain non-display controls`() {
        val lite = liteStatus()
        assertFalse(badgeDisplayControlsAvailable(lite))
        listOf(
            "badge_display_policy",
            "badge_display_policy_reset",
            "badge_theme",
            "badge_theme_reset",
            "display_nav",
        ).forEach { command ->
            assertFalse(
                command,
                BadgeControlTransportPolicy.allowsAndroidControlCommand(command, lite),
            )
        }
        assertTrue(BadgeControlTransportPolicy.allowsAndroidControlCommand("set_mode", lite))
        assertTrue(BadgeControlTransportPolicy.allowsAndroidControlCommand("reboot", lite))
        assertTrue(badgeDisplayControlsAvailable(BadgeControlStatus(mode = "backend")))
    }

    private fun liteStatus(): BadgeControlStatus = BadgeControlStatus(
        version = "0.2.0-backend",
        firmwareTarget = "uplink-s3-backend",
        productFamily = "badge_lite",
        project = "fof_backend_uplink",
        hardware = "seeed_xiao_esp32s3",
        mac = "A4:CF:12:34:56:78",
        mode = "headless",
        modeLabel = "Backend Badge Lite",
        capabilities = setOf("display_none", "usb_live", "usb_live_ack"),
    )

    private fun attachmentToken() = BadgeUsbAttachmentToken(
        generation = 11L,
        identity = BadgeUsbDeviceIdentity(101, "/dev/badge"),
    )

    private fun owner(kind: BadgeUsbProductKind): BadgeUsbOwnerKey = BadgeUsbOwnerKey(
        attachmentToken = attachmentToken(),
        lifecycleSession = 7L,
        connectionIdentity = Any(),
        endpointIdentity = Any(),
        hardwareId = "A4:CF:12:34:56:78",
        productKind = kind,
    )
}
