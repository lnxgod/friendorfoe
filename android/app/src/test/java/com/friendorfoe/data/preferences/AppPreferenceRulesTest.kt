package com.friendorfoe.data.preferences

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class AppPreferenceRulesTest {

    @Test
    fun invalidTopLevelRouteFallsBackToAbout() {
        assertEquals("info", sanitizeTopLevelRoute("calibrate"))
        assertEquals("info", sanitizeTopLevelRoute(null))
    }

    @Test
    fun normalLaunchAlwaysUsesAboutInsteadOfTheSavedTopLevelRoute() {
        assertEquals("privacy", sanitizeTopLevelRoute("privacy"))
        assertEquals("info", normalLaunchRoute())
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
        assertNull(FindingPreferenceKey.decode("phone_ble\u001Fstable\u001Fextra"))
        assertNull(FindingPreferenceKey.create("phone\u001Fble", "stable"))
        assertNull(FindingPreferenceKey.create("phone_ble", "stable\u001Fextra"))
    }
}
