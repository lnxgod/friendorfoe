package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BleInvestigationModelsTest {

    @Test
    fun `badge investigation chunks assemble in sequence`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(
            BleInvestigationChunk.Begin(
                "req-1",
                BleInvestigationMode.GATT,
                "AA:BB:CC:DD:EE:FF",
            ),
        )
        assembler.accept(BleInvestigationChunk.Progress("req-1", BleInvestigationState.DISCOVERING))
        assembler.accept(BleInvestigationChunk.Service("req-1", 0, "FFE0"))
        assembler.accept(
            BleInvestigationChunk.Characteristic(
                "req-1",
                0,
                "FFE0",
                "FFE1",
                setOf("read", "write"),
            ),
        )
        assembler.accept(BleInvestigationChunk.Read("req-1", 0, "FFE1", "4142"))

        val result = assembler.accept(
            BleInvestigationChunk.End("req-1", "complete", "UART service found"),
        )

        assertEquals(BleInvestigationState.COMPLETE, result!!.state)
        assertEquals(listOf("FFE0"), result.services)
        assertEquals("FFE1", result.characteristics.single().uuid)
        assertEquals(setOf("read", "write"), result.characteristics.single().properties)
        assertEquals(mapOf("FFE1" to "4142"), result.reads)
        assertFalse(result.truncated)
    }

    @Test
    fun `out of order request id is rejected without corrupting active result`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))
        assembler.accept(BleInvestigationChunk.Service("req-1", 0, "1800"))

        assertNull(assembler.accept(BleInvestigationChunk.Service("req-2", 1, "BAD0")))
        assertNull(
            assembler.accept(
                BleInvestigationChunk.End("req-2", "failed", "wrong request"),
            ),
        )
        assertNull(assembler.accept(BleInvestigationChunk.Service("req-1", 1, "180A")))

        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(listOf("1800", "180A"), result!!.services)
        assertEquals(BleInvestigationState.COMPLETE, result.state)
    }

    @Test
    fun `out of order indexes are rejected without advancing the sequence`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))

        assertNull(assembler.accept(BleInvestigationChunk.Service("req-1", 1, "1801")))
        assertNull(assembler.accept(BleInvestigationChunk.Service("req-1", 0, "1800")))

        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(listOf("1800"), result!!.services)
    }

    @Test
    fun `only the first begin chunk is accepted`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(
            BleInvestigationChunk.Begin(
                "req-1",
                BleInvestigationMode.GATT,
                "AA:BB:CC:DD:EE:FF",
            ),
        )
        assembler.accept(BleInvestigationChunk.Service("req-1", 0, "1800"))

        assertNull(
            assembler.accept(
                BleInvestigationChunk.Begin(
                    "req-1",
                    BleInvestigationMode.PASSIVE_CAPTURE,
                    "11:22:33:44:55:66",
                ),
            ),
        )

        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(BleInvestigationMode.GATT, result!!.mode)
        assertEquals("AA:BB:CC:DD:EE:FF", result.targetMac)
        assertEquals(listOf("1800"), result.services)
    }

    @Test
    fun `terminal assembler rejects every later chunk including repeated end`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))
        assembler.accept(BleInvestigationChunk.Service("req-1", 0, "1800"))
        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(BleInvestigationState.COMPLETE, result!!.state)
        assertNull(assembler.accept(BleInvestigationChunk.End("req-1", "failed", "again")))
        assertNull(
            assembler.accept(
                BleInvestigationChunk.Begin(
                    "req-1",
                    BleInvestigationMode.PASSIVE_CAPTURE,
                    "11:22:33:44:55:66",
                ),
            ),
        )
        assertNull(assembler.accept(BleInvestigationChunk.Progress("req-1", BleInvestigationState.READING)))
        assertNull(assembler.accept(BleInvestigationChunk.Service("req-1", 0, "BAD0")))
        assertNull(
            assembler.accept(
                BleInvestigationChunk.Characteristic(
                    "req-1",
                    0,
                    "BAD0",
                    "BAD1",
                    setOf("read"),
                ),
            ),
        )
        assertNull(assembler.accept(BleInvestigationChunk.Read("req-1", 0, "BAD1", "00")))
        assertNull(assembler.accept(BleInvestigationChunk.End("req-1", "complete", "reopened")))
    }

    @Test
    fun `service characteristic and read limits set truncation flag`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))

        repeat(17) { index ->
            assembler.accept(BleInvestigationChunk.Service("req-1", index, "service-$index"))
        }
        repeat(33) { index ->
            assembler.accept(
                BleInvestigationChunk.Characteristic(
                    "req-1",
                    index,
                    "service-0",
                    "char-$index",
                    setOf("read"),
                ),
            )
        }
        repeat(9) { index ->
            assembler.accept(BleInvestigationChunk.Read("req-1", index, "read-$index", "00"))
        }

        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(16, result!!.services.size)
        assertEquals(32, result.characteristics.size)
        assertEquals(8, result.reads.size)
        assertTrue(result.truncated)
    }

    @Test
    fun `read values are capped at sixty four bytes of hex`() {
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))
        assembler.accept(BleInvestigationChunk.Read("req-1", 0, "FFE1", "AB".repeat(65)))

        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals("AB".repeat(64), result!!.reads.getValue("FFE1"))
        assertTrue(result.truncated)
    }

    @Test
    fun `characteristic properties are snapshotted when accepted`() {
        val properties = mutableSetOf("read")
        val assembler = BleInvestigationChunkAssembler("req-1")
        assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, null))
        assembler.accept(
            BleInvestigationChunk.Characteristic(
                "req-1",
                0,
                "FFE0",
                "FFE1",
                properties,
            ),
        )

        properties += "write"
        val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "done"))

        assertEquals(setOf("read"), result!!.characteristics.single().properties)
    }

    @Test
    fun `normalized contract retains exact state names and request defaults`() {
        assertEquals(
            listOf(
                "IDLE",
                "QUEUED",
                "SCANNING",
                "CONNECTING",
                "DISCOVERING",
                "READING",
                "COMPLETE",
                "FAILED",
                "CANCELLED",
            ),
            BleInvestigationState.entries.map { it.name },
        )
        assertEquals(listOf("AUTO", "PHONE", "BADGE"), BleInvestigationRoute.entries.map { it.name })

        val request = BleInvestigationRequest(
            requestId = "req-1",
            target = BleInvestigationTarget(
                mode = BleInvestigationMode.PASSIVE_CAPTURE,
                mac = null,
                entityKey = "pairing-spam",
                observedAtElapsedMs = 100,
                origin = PrivacyDetectionOrigin.BADGE,
            ),
            route = BleInvestigationRoute.AUTO,
        )

        assertEquals(12_000L, request.timeoutMs)
    }
}
