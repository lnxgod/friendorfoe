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
    fun policyClassInfoKeysAreUniqueAndMatchDefaults() {
        val infoKeys = BadgeDisplayPolicyClasses.map { it.key }
        val defaultKeys = defaultBadgeDisplayPolicy().classes.keys

        assertEquals(infoKeys.distinct(), infoKeys)
        assertEquals(defaultKeys, infoKeys.toSet())
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

    @Test
    fun defaultThemeContainsSafeAccentDefaults() {
        val theme = defaultBadgeTheme()

        assertEquals("field", theme.palette)
        assertEquals("dark", theme.background)
        assertEquals(100, theme.brightness)
        assertEquals(BadgeThemeAccentClasses.map { it.key }.toSet(), theme.accents.keys)
        assertEquals(BadgeThemeAccentClasses.map { it.key }.distinct(),
            BadgeThemeAccentClasses.map { it.key })
    }

    @Test
    fun themeCommandJsonBuildsExpectedBadgeControlPayload() {
        val theme = defaultBadgeTheme().copy(
            palette = "night",
            background = "scanline",
            brightness = 70,
            accents = defaultBadgeThemeAccents() + ("meta" to 0xF800)
        )

        val json = JsonParser.parseString(
            badgeThemeCommandJson(theme, persist = true).toString()
        ).asJsonObject

        assertEquals("badge_theme", json.get("cmd").asString)
        assertTrue(json.get("persist").asBoolean)
        val themeObj = json.getAsJsonObject("theme")
        assertEquals("night", themeObj.get("palette").asString)
        assertEquals("scanline", themeObj.get("background").asString)
        assertEquals(70, themeObj.get("brightness").asInt)
        assertEquals(0xF800, themeObj.getAsJsonObject("accents").get("meta").asInt)
    }
}
