package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext

class BadgeUsbActivityTest {

    @Test
    fun `stable key prefers entity then id`() {
        assertEquals("entity:drone:abc", detection(entity = "drone:abc").stableKey)
        assertEquals("id:rid-7", detection(id = "rid-7").stableKey)
    }

    @Test
    fun `activity is newest first deduplicated and bounded`() {
        val result = (1L..70L).fold(emptyList<BadgeUsbActivity>()) { current, n ->
            pushBadgeUsbActivity(
                current,
                BadgeUsbActivity(BadgeUsbActivityKind.DETECTION, "event:$n", "Drone $n", "", n),
                limit = 64,
            )
        }
        assertEquals(64, result.size)
        assertEquals(70L, result.first().receivedAtElapsedMs)
        assertEquals(7L, result.last().receivedAtElapsedMs)

        val replaced = pushBadgeUsbActivity(
            result,
            result.first().copy(title = "Updated"),
            limit = 64,
        )
        assertEquals(64, replaced.size)
        assertEquals("Updated", replaced.first().title)
        assertEquals(1, replaced.count { it.key == "event:70" })
    }

    @Test
    fun `activity classification keeps only supported badge frames`() {
        val detection = detection(id = "rid-7", entity = "drone:abc").copy(
            receivedAtElapsedMs = 91L,
        )

        assertNull(badgeUsbActivityForLine("I (42) wifi: noisy serial log", 100L))
        assertNull(badgeUsbActivityForLine("FOF_DET:not-json", 100L))

        val detectionActivity = badgeUsbActivityForLine(
            "FOF_DET:{...}",
            100L,
            detection = detection,
        )
        assertEquals(BadgeUsbActivityKind.DETECTION, detectionActivity?.kind)
        assertEquals("entity:drone:abc", detectionActivity?.key)
        assertEquals(91L, detectionActivity?.receivedAtElapsedMs)

        assertEquals(
            BadgeUsbActivityKind.STATUS,
            badgeUsbActivityForLine(
                "FOF_STATUS:{...}",
                101L,
                status = BadgeControlStatus(version = "1.2.3"),
            )?.kind,
        )
        listOf("FOF_FW_UPLOAD:{...}", "FOF_FW_RELAY_PROGRESS:{...}", "FOF_FW_RELAY:{...}")
            .forEach { line ->
                assertEquals(
                    BadgeUsbActivityKind.FIRMWARE,
                    badgeUsbActivityForLine(
                        line,
                        102L,
                        firmwareProgress = BadgeFirmwareProgress(kind = "relay", stage = "writing"),
                    )?.kind,
                )
            }
        assertEquals(
            BadgeUsbActivityKind.COMMAND,
            badgeUsbActivityForLine("FOF_CTL_OK:{\"ok\":true}", 103L)?.kind,
        )
        assertEquals(
            BadgeUsbActivityKind.ERROR,
            badgeUsbActivityForLine("FOF_CTL_ERROR:{\"error\":\"busy\"}", 104L)?.kind,
        )
        assertEquals(
            BadgeUsbActivityKind.STATUS,
            badgeUsbActivityForLine(
                "FOF_INV:{\"request_id\":\"r1\"}",
                105L,
                investigationHandled = true,
            )?.kind,
        )
        assertNull(badgeUsbActivityForLine("FOF_INV:{\"request_id\":\"r1\"}", 106L))
    }

    @Test
    fun `atomic state flow updates retain concurrent increments`() = runBlocking {
        val state = MutableStateFlow(0)
        withContext(Dispatchers.Default) {
            (1..64).map {
                async {
                    repeat(100) { state.update { current -> current + 1 } }
                }
            }.awaitAll()
        }

        assertEquals(6400, state.value)
    }

    private fun detection(
        id: String = "",
        entity: String = "",
    ) = BadgeUsbDetection(
        id = id,
        manufacturer = "DJI",
        badgeEntityKey = entity,
        source = 0,
        confidence = 0.9f,
        rssi = -45,
        receivedAtElapsedMs = 1L,
    )
}
