package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeLiteUsbProtocolTest {

    @Test
    fun `parses only the exact acknowledged live contract`() {
        assertEquals(
            BadgeLiteLiveFrame.Ready("boot-a1", 5_000, 15_000L),
            parseBadgeLiteLiveFrame(
                "FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":5000,\"lease_ms\":15000}",
            ),
        )
        assertEquals(
            BadgeLiteLiveFrame.Heartbeat("boot-a1", 18446744073709551615u),
            parseBadgeLiteLiveFrame(
                "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":18446744073709551615}",
            ),
        )
        assertEquals(
            BadgeLiteLiveFrame.Stopped("boot-a1"),
            parseBadgeLiteLiveFrame("FOF_LIVE_STOPPED:{\"session_id\":\"boot-a1\"}"),
        )

        listOf(
            "FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":1000,\"lease_ms\":15000}",
            "FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":5000,\"lease_ms\":15000,\"extra\":true}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":-1}",
            "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":\"1\"}",
            "FOF_LIVE_STOPPED:{\"session_id\":\"bad\\nvalue\"}",
        ).forEach { line -> assertNull(line, parseBadgeLiteLiveFrame(line)) }
    }

    @Test
    fun `live wires preserve the exact compact serial protocol`() {
        assertEquals(
            "FOF_LIVE_START:{\"client\":\"android\",\"protocol\":1}",
            badgeLiteLiveStartWire(),
        )
        assertEquals(
            "FOF_LIVE_ACK:{\"session_id\":\"boot-a1\",\"sequence\":7}",
            badgeLiteLiveAckWire("boot-a1", 7u),
        )
        assertTrue(badgeLiteLiveAckWire("quoted\\\"id", 1u).contains("\\\\\\\""))
    }
}
