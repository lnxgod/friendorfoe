package com.friendorfoe.data.badge

import com.google.gson.JsonParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeDisplayPolicyTest {

    @Test
    fun defaultPolicyContainsBadgeClassDefaults() {
        val policy = defaultBadgeDisplayPolicy()

        assertEquals("both", policy.classes.getValue("drone").lane)
        assertEquals("both", policy.classes.getValue("meta").lane)
        assertEquals("close", policy.classes.getValue("hid").minProximity)
        assertEquals(13, policy.classes.size)
    }

    @Test
    fun commandJsonBuildsExpectedBadgeControlPayload() {
        val policy = defaultBadgeDisplayPolicy().let {
            it.copy(
                classes = it.classes + (
                    "beacon" to it.classes.getValue("beacon").copy(enabled = false)
                )
            )
        }

        val obj = badgeDisplayPolicyCommandJson(policy, persist = true)
        val json = JsonParser.parseString(obj.toString()).asJsonObject

        assertEquals("badge_display_policy", json.get("cmd").asString)
        assertTrue(json.get("persist").asBoolean)
        val beacon = json.getAsJsonObject("policy")
            .getAsJsonObject("classes")
            .getAsJsonObject("beacon")
        assertFalse(beacon.get("enabled").asBoolean)
        assertEquals("lower", beacon.get("lane").asString)
    }

    @Test
    fun displayNavCommandBuildsExpectedBadgeControlPayload() {
        val json = JsonParser.parseString(
            badgeDisplayNavCommandJson("next").toString()
        ).asJsonObject

        assertEquals("display_nav", json.get("cmd").asString)
        assertEquals("next", json.get("action").asString)
    }
}
