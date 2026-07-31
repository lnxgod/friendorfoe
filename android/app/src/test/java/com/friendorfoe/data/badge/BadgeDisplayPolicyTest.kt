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
        assertEquals(14, policy.classes.size)
        assertEquals(
            BadgeDisplayClassPolicy(
                enabled = false,
                lane = "off",
                minProximity = "close",
                priority = 0,
            ),
            policy.classes.getValue("skimmer"),
        )

        val bleAttackInfo = BadgeDisplayPolicyClasses.single { it.key == "ble_attack" }
        assertEquals("ble_attack", bleAttackInfo.key)
        assertEquals("BLE Attack", bleAttackInfo.label)

        val bleAttack = policy.classes.getValue(bleAttackInfo.key)
        assertTrue(bleAttack.enabled)
        assertEquals("both", bleAttack.lane)
        assertEquals("present", bleAttack.minProximity)
        assertEquals(92, bleAttack.priority)
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
    fun partialAndMalformedPolicyMapsSerializeAsOneCompleteCanonicalPolicy() {
        val malformed = BadgeDisplayPolicy(
            version = 99,
            classes = linkedMapOf(
                "drone" to BadgeDisplayClassPolicy(
                    enabled = true,
                    lane = "INVALID",
                    minProximity = "far",
                    priority = 999,
                ),
                "ignored" to BadgeDisplayClassPolicy(),
            ),
        )

        val policy = badgeDisplayPolicyCommandJson(malformed)
            .getAsJsonObject("policy")
        val classes = policy.getAsJsonObject("classes")
        assertEquals(1, policy.get("version").asInt)
        assertEquals(
            BadgeDisplayPolicyClasses.map { it.key },
            classes.entrySet().map { it.key },
        )
        assertFalse(classes.has("ignored"))
        assertEquals(
            defaultBadgeDisplayPolicy().classes.getValue("drone").lane,
            classes.getAsJsonObject("drone").get("lane").asString,
        )
        assertEquals(
            defaultBadgeDisplayPolicy().classes.getValue("drone").minProximity,
            classes.getAsJsonObject("drone").get("min_proximity").asString,
        )
        assertEquals(100, classes.getAsJsonObject("drone").get("priority").asInt)
        assertEquals(
            false,
            classes.getAsJsonObject("skimmer").get("enabled").asBoolean,
        )
    }

    @Test
    fun disablingDroneClassSendsExplicitOffLane() {
        val policy = defaultBadgeDisplayPolicy().withClassEnabled("drone", enabled = false)

        val drone = policy.classes.getValue("drone")
        assertFalse(drone.enabled)
        assertEquals("off", drone.lane)

        val json = JsonParser.parseString(
            badgeDisplayPolicyCommandJson(policy, persist = true).toString()
        ).asJsonObject

        val droneJson = json.getAsJsonObject("policy")
            .getAsJsonObject("classes")
            .getAsJsonObject("drone")
        assertFalse(droneJson.get("enabled").asBoolean)
        assertEquals("off", droneJson.get("lane").asString)
    }

    @Test
    fun enablingDroneClassRestoresDefaultLaneAfterBeingOff() {
        val disabled = defaultBadgeDisplayPolicy().withClassEnabled("drone", enabled = false)
        val enabled = disabled.withClassEnabled("drone", enabled = true)
        val defaultDrone = defaultBadgeDisplayPolicy().classes.getValue("drone")

        assertTrue(enabled.classes.getValue("drone").enabled)
        assertEquals(defaultDrone.lane, enabled.classes.getValue("drone").lane)
        assertEquals(defaultDrone.minProximity, enabled.classes.getValue("drone").minProximity)
    }

    @Test
    fun displayNavCommandsMatchFrozenBadgeFirmwareContract() {
        val expected = listOf(
            BadgeDisplayNavAction.NEXT to "next",
            BadgeDisplayNavAction.DETAIL to "detail",
            BadgeDisplayNavAction.PAGE to "page",
            BadgeDisplayNavAction.BACK to "back",
        )

        assertEquals(expected.map { it.first }, BadgeDisplayNavAction.entries)
        expected.forEach { (action, wireValue) ->
            val json = JsonParser.parseString(
                badgeDisplayNavCommandJson(action).toString()
            ).asJsonObject

            assertEquals("display_nav", json.get("cmd").asString)
            assertEquals(wireValue, json.get("action").asString)
        }
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
