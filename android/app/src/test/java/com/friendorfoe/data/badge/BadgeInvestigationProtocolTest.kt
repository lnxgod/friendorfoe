package com.friendorfoe.data.badge

import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationChunk
import com.friendorfoe.detection.BleInvestigationState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

class BadgeInvestigationProtocolTest {

    @Test
    fun `FOF INV lines assemble every badge chunk into a result`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")

        assertAccepted(parser, begin("r1"))
        assertAccepted(parser, inv("""{"type":"ble_inv_progress","request_id":"r1","state":"discovering"}"""))
        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"FFE0"}"""))
        assertAccepted(
            parser,
            inv("""{"type":"ble_inv_char","request_id":"r1","index":0,"service_uuid":"FFE0","uuid":"FFE1","properties":["read","notify"]}"""),
        )
        assertAccepted(parser, inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"FFE1","value_hex":"4142"}"""))

        val result = parser.accept(end("r1", summary = "UART service found")).result

        assertEquals(BleInvestigationState.COMPLETE, result!!.state)
        assertEquals(listOf("FFE0"), result.services)
        assertEquals("FFE1", result.characteristics.single().uuid)
        assertEquals(setOf("read", "notify"), result.characteristics.single().properties)
        assertEquals(mapOf("FFE1" to "4142"), result.reads)
        assertEquals("UART service found", result.summary)
    }

    @Test
    fun `malformed JSON and malformed required fields are rejected without mutation`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")

        listOf(
            "FOF_STATUS:{}",
            "FOF_INV:not-json",
            inv("""[]"""),
            inv("""{"type":"ble_inv_begin","request_id":"","mode":"gatt"}"""),
            inv("""{"type":"ble_inv_begin","request_id":"r1","mode":"write"}"""),
            inv("""{"type":"ble_inv_begin","request_id":"r1","mode":"gatt","target_mac":4}"""),
            begin("r1", target = "AA:BB:CC:DD:EE"),
            begin("r1", target = null),
            begin("r1", mode = "passive_capture", target = "AA:BB:CC:DD:EE:FF"),
            begin("x".repeat(33)),
            inv("""{"type":"ble_inv_begin","request_id":"r1\u0001","mode":"gatt","target_mac":null}"""),
        ).forEach { assertRejected(parser, it) }

        assertAccepted(parser, begin("r1"))
        listOf(
            inv("""{"type":"ble_inv_progress","request_id":"r1","state":"complete"}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":-1,"uuid":"1800"}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":""}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":0.5,"uuid":"1800"}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":2147483648,"uuid":"1800"}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"service-0"}"""),
            inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"ffe0"}"""),
            inv("""{"type":"ble_inv_char","request_id":"r1","index":0,"service_uuid":"1800","uuid":"2A00","properties":"read"}"""),
            inv("""{"type":"ble_inv_char","request_id":"r1","index":0,"service_uuid":"1800","uuid":"2A00","properties":["execute"]}"""),
            inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"2A00","value_hex":"XYZ"}"""),
            inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"2A00"}"""),
            inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"2A00","value_hex":3}"""),
            inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"2A00","value_hex":"${"AB".repeat(65)}"}"""),
            inv("""{"type":"ble_inv_end","request_id":"r1","state":"reading","summary":"no"}"""),
            inv("""{"type":"ble_inv_end","request_id":"r1","state":"complete"}"""),
            inv("""{"type":"ble_inv_end","request_id":"r1","state":"complete","summary":3}"""),
            end("r1", summary = "S".repeat(128)),
        ).forEach { assertRejected(parser, it) }

        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"1800"}"""))
        val result = parser.accept(end("r1")).result
        assertEquals(listOf("1800"), result!!.services)
    }

    @Test
    fun `mismatched request cannot reset or overwrite active matching assembly`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")
        assertAccepted(parser, begin("r1", target = "AA:BB:CC:DD:EE:FF"))
        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"1800"}"""))

        assertRejected(parser, begin("other", mode = "passive_capture"))
        assertRejected(parser, inv("""{"type":"ble_inv_service","request_id":"other","index":1,"uuid":"BAD0"}"""))
        assertRejected(parser, end("other", state = "failed", summary = "wrong"))

        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":1,"uuid":"180A"}"""))
        val result = parser.accept(end("r1")).result
        assertEquals(BleInvestigationMode.GATT, result!!.mode)
        assertEquals("AA:BB:CC:DD:EE:FF", result.targetMac)
        assertEquals(listOf("1800", "180A"), result.services)
    }

    @Test
    fun `duplicate and out of order chunks do not advance indexes`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")
        assertAccepted(parser, begin("r1"))

        assertRejected(parser, begin("r1"))
        assertRejected(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":1,"uuid":"1801"}"""))
        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"1800"}"""))
        assertRejected(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"DUP0"}"""))
        assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":1,"uuid":"180A"}"""))

        val result = parser.accept(end("r1")).result
        assertEquals(listOf("1800", "180A"), result!!.services)
        assertRejected(parser, end("r1", state = "failed", summary = "duplicate terminal"))
    }

    @Test
    fun `bounded assembly reports local and remote truncation`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")
        assertAccepted(parser, begin("r1"))
        repeat(17) { index ->
            val uuid = canonicalUuid(index)
            assertAccepted(parser, inv("""{"type":"ble_inv_service","request_id":"r1","index":$index,"uuid":"$uuid"}"""))
        }
        repeat(9) { index ->
            val uuid = "%08X".format(0x2A00 + index)
            assertAccepted(
                parser,
                inv("""{"type":"ble_inv_read","request_id":"r1","index":$index,"uuid":"$uuid","value_hex":"4142"}"""),
            )
        }

        val result = parser.accept(end("r1", truncated = true)).result

        assertEquals(16, result!!.services.size)
        assertEquals(8, result.reads.size)
        assertTrue(result.truncated)
        assertEquals("4142", result.reads.getValue("00002A00"))
    }

    @Test
    fun `accepted progress and data chunks are exposed before terminal result`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")

        val begin = parser.accept(begin("r1"))
        val progress = parser.accept(
            inv("""{"type":"ble_inv_progress","request_id":"r1","state":"connecting"}"""),
        )
        val service = parser.accept(
            inv("""{"type":"ble_inv_service","request_id":"r1","index":0,"uuid":"1800"}"""),
        )

        assertTrue(begin.chunk is BleInvestigationChunk.Begin)
        assertEquals(BleInvestigationState.CONNECTING, (progress.chunk as BleInvestigationChunk.Progress).state)
        assertEquals("1800", (service.chunk as BleInvestigationChunk.Service).uuid)
        assertNull(progress.result)
        assertNull(service.result)
    }

    @Test
    fun `wire bounds match firmware JSON and FOF INV frame contract`() {
        assertEquals(1023, BADGE_INVESTIGATION_JSON_MAX_CHARS)
        assertEquals(1031, BADGE_INVESTIGATION_LINE_MAX_CHARS)

        val base = begin("r1")
        val exactBoundary = base + " ".repeat(BADGE_INVESTIGATION_LINE_MAX_CHARS - base.length)
        assertAccepted(BadgeInvestigationStreamParser("r1"), exactBoundary)
        assertRejected(BadgeInvestigationStreamParser("r1"), "$exactBoundary ")
    }

    @Test
    fun `zero length read and empty terminal summary are valid`() {
        val parser = BadgeInvestigationStreamParser("r1")
        assertAccepted(parser, begin("r1"))
        assertAccepted(
            parser,
            inv("""{"type":"ble_inv_read","request_id":"r1","index":0,"uuid":"2A00","value_hex":""}"""),
        )

        val result = parser.accept(end("r1", summary = "")).result

        assertEquals("", result!!.reads.getValue("2A00"))
        assertEquals("", result.summary)
    }

    @Test
    fun `USB acknowledgement requires matching BEGIN and ignores generic replies`() {
        val r1Parser = BadgeInvestigationStreamParser("r1")
        val r2Parser = BadgeInvestigationStreamParser("r2")
        val r1Gate = BadgeUsbInvestigationAckGate("r1")
        val r2Gate = BadgeUsbInvestigationAckGate("r2")
        val genericOk = "FOF_CTL_OK:{\"message\":\"BLE investigation started\"}"

        assertNull(r1Gate.accept(genericOk, parsed = null, ownsControlReply = true))
        val r2Begin = begin("r2")
        val rejectedByR1 = r1Parser.accept(r2Begin)
        val acceptedByR2 = r2Parser.accept(r2Begin)
        assertNull(r1Gate.accept(r2Begin, rejectedByR1, ownsControlReply = true))
        assertEquals(true, r2Gate.accept(r2Begin, acceptedByR2, ownsControlReply = true)?.accepted)

        val error = "FOF_CTL_ERROR:{\"error\":\"scanner_unavailable\"}"
        assertNull(BadgeUsbInvestigationAckGate("r3").accept(error, null, ownsControlReply = false))
        assertNull(BadgeUsbInvestigationAckGate("r3").accept(error, null, ownsControlReply = true))
    }

    @Test
    fun `GATT callbacks require matching connection kind UUID and generation`() {
        val expectedGatt = Any()
        val otherGatt = Any()
        val uuid = UUID.fromString("0000ff03-0000-1000-8000-00805f9b34fb")

        assertTrue(
            badgeGattCallbackMatches(
                expectedGatt, expectedGatt, "read", "read", uuid, uuid, 7L, 7L,
            ),
        )
        assertFalse(
            badgeGattCallbackMatches(
                expectedGatt, otherGatt, "read", "read", uuid, uuid, 7L, 7L,
            ),
        )
        assertFalse(
            badgeGattCallbackMatches(
                expectedGatt, expectedGatt, "read", "write", uuid, uuid, 7L, 7L,
            ),
        )
        assertFalse(
            badgeGattCallbackMatches(
                expectedGatt, expectedGatt, "read", "read", uuid, UUID.randomUUID(), 7L, 7L,
            ),
        )
        assertFalse(
            badgeGattCallbackMatches(
                expectedGatt, expectedGatt, "read", "read", uuid, uuid, 7L, 8L,
            ),
        )
        assertTrue(
            badgeGattCallbackMatches(
                expectedGatt, expectedGatt, "read", "read", uuid, uuid, null, 8L,
            ),
        )
        assertTrue(badgeGattDisconnectMatches(expectedGatt, expectedGatt))
        assertFalse(badgeGattDisconnectMatches(expectedGatt, otherGatt))
    }

    @Test
    fun `transport job starts only while its generation remains active`() {
        assertTrue(shouldStartBadgeInvestigationJob(activeGeneration = 4L, operationGeneration = 4L))
        assertFalse(shouldStartBadgeInvestigationJob(activeGeneration = null, operationGeneration = 4L))
        assertFalse(shouldStartBadgeInvestigationJob(activeGeneration = 5L, operationGeneration = 4L))
    }

    @Test
    fun `disconnect terminates only matching active request and reconnect accepts replay`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "r1")
        assertAccepted(parser, begin("r1"))
        assertNull(parser.disconnect("other"))

        val disconnected = parser.disconnect("r1")
        assertEquals(BleInvestigationState.FAILED, disconnected!!.state)
        assertEquals("transport_disconnected", disconnected.error)
        assertRejected(parser, end("r1"))

        parser.reconnect("r1")
        assertAccepted(parser, begin("r1"))
        val replayed = parser.accept(end("r1", state = "failed", summary = "Scanner timed out", error = "timeout")).result
        assertEquals(BleInvestigationState.FAILED, replayed!!.state)
        assertEquals("timeout", replayed.error)
    }

    @Test
    fun `terminal timeout replay BEGIN plus END is accepted after reset`() {
        val parser = BadgeInvestigationStreamParser(expectedRequestId = "timeout-1")
        parser.reconnect("timeout-1")

        assertAccepted(parser, begin("timeout-1", mode = "passive_capture", target = null))
        val result = parser.accept(
            end(
                requestId = "timeout-1",
                state = "failed",
                summary = "Investigation timed out",
                error = "timeout",
                truncated = true,
            ),
        ).result

        assertEquals(BleInvestigationMode.PASSIVE_CAPTURE, result!!.mode)
        assertEquals(BleInvestigationState.FAILED, result.state)
        assertEquals("timeout", result.error)
        assertTrue(result.truncated)
    }

    private fun assertAccepted(parser: BadgeInvestigationStreamParser, line: String) {
        val accepted = parser.accept(line)
        assertTrue("Expected accepted line: $line", accepted.accepted)
        assertTrue("Expected accepted chunk: $line", accepted.chunk != null)
        assertNull(accepted.result)
    }

    private fun assertRejected(parser: BadgeInvestigationStreamParser, line: String) {
        val accepted = parser.accept(line)
        assertFalse("Expected rejected line: $line", accepted.accepted)
        assertNull(accepted.chunk)
        assertNull(accepted.result)
    }

    private fun inv(json: String) = "FOF_INV:$json"

    private fun canonicalUuid(index: Int): String =
        "00000000-0000-1000-8000-${index.toString(16).uppercase().padStart(12, '0')}"

    private fun begin(
        requestId: String,
        mode: String = "gatt",
        target: String? = "AA:BB:CC:DD:EE:FF",
    ): String = inv(
        """{"type":"ble_inv_begin","request_id":"$requestId","mode":"$mode","target_mac":${target?.let { "\"$it\"" } ?: "null"}}""",
    )

    private fun end(
        requestId: String,
        state: String = "complete",
        summary: String = "done",
        error: String? = null,
        truncated: Boolean = false,
    ): String = inv(
        """{"type":"ble_inv_end","request_id":"$requestId","state":"$state","summary":"$summary","error":${error?.let { "\"$it\"" } ?: "null"},"authentication_required":false,"truncated":$truncated}""",
    )
}
