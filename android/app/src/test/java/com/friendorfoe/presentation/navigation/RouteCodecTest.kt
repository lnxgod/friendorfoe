package com.friendorfoe.presentation.navigation

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class RouteCodecTest {
    @Test
    fun routeEncodingPreservesLegacyFormEncoderCharactersWithoutPlusSpaces() {
        assertEquals(
            "a%20b*%7E%2F%E2%98%83",
            encodeRouteSegment("a b*~/☃"),
        )
    }

    @Test
    fun routeSegmentsRoundTripWithoutTreatingPlusAsSpace() {
        val original = "phone_ble/entity:42 + snowman ☃"

        assertEquals(original, decodeRouteSegment(encodeRouteSegment(original)))
        assertEquals("a+b", decodeRouteSegment("a+b"))
        assertNull(decodeRouteSegment("bad%2"))
        assertNull(decodeRouteSegment("bad%GG"))
    }
}
