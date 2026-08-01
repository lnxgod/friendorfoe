package com.friendorfoe.data.preferences

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class AppPreferenceRulesTest {

    @Test
    fun invalidTopLevelRouteFallsBackToAr() {
        assertEquals("ar_view", sanitizeTopLevelRoute("calibrate"))
        assertEquals("ar_view", sanitizeTopLevelRoute(null))
    }

    @Test
    fun sourceAndStableIdBothParticipateInIgnoredKey() {
        assertNotEquals(
            FindingPreferenceKey.create("phone_ble", "AA:BB")!!.encoded,
            FindingPreferenceKey.create("badge", "AA:BB")!!.encoded
        )
    }

    @Test
    fun unstableIdentityCannotBePersisted() {
        assertNull(FindingPreferenceKey.create("backend", ""))
    }

    @Test
    fun findingKeyRoundTripsAndMalformedValuesAreRejected() {
        val key = FindingPreferenceKey.create("phone_ble", "AA:BB")!!
        assertEquals(key, FindingPreferenceKey.decode(key.encoded))
        assertNull(FindingPreferenceKey.decode("missing-separator"))
        assertNull(FindingPreferenceKey.decode("phone_ble\u001F"))
        assertNull(FindingPreferenceKey.decode("\u001FAA:BB"))
    }
}
