package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Test

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
