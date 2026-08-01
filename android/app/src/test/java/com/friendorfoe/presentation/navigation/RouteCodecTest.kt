package com.friendorfoe.presentation.navigation

import org.junit.Assert.assertEquals
import org.junit.Test

class RouteCodecTest {
    @Test
    fun routeEncodingPreservesLegacyFormEncoderCharactersWithoutPlusSpaces() {
        assertEquals(
            "a%20b*%7E%2F%E2%98%83",
            encodeRouteSegment("a b*~/☃"),
        )
    }
}
