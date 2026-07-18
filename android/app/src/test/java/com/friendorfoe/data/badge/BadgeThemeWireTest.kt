package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeThemeWireTest {

    @Test
    fun `wire palette and accent tokens remain frozen`() {
        assertEquals(listOf("field", "night", "neon", "mono"), BadgeThemePalettes)
        assertEquals(
            listOf("drone", "meta", "tracker", "flock", "wifi_attack", "clear"),
            BadgeThemeAccentClasses.map { it.key },
        )
    }

    @Test
    fun `every preset serializes as a complete version one badge theme`() {
        BadgeThemePresets.forEach { preset ->
            val command = badgeThemeCommandJson(preset.theme, persist = true)
            val themeJson = command.getAsJsonObject("theme")

            assertEquals("badge_theme", command.get("cmd").asString)
            assertTrue(command.get("persist").asBoolean)
            assertEquals(listOf("version", "palette", "background", "brightness", "accents"),
                themeJson.entrySet().map { it.key })
            assertEquals(1, themeJson.get("version").asInt)
            assertTrue(themeJson.get("palette").asString in BadgeThemePalettes)
            assertEquals(preset.theme.palette, themeJson.get("palette").asString)
            assertEquals(preset.theme.background, themeJson.get("background").asString)
            assertEquals(preset.theme.brightness, themeJson.get("brightness").asInt)

            val accentsJson = themeJson.getAsJsonObject("accents")
            assertEquals(BadgeThemeAccentClasses.map { it.key }, accentsJson.entrySet().map { it.key })
            BadgeThemeAccentClasses.forEach { accent ->
                assertEquals(
                    preset.theme.accents.getValue(accent.key),
                    accentsJson.get(accent.key).asInt,
                )
            }

            assertFalse(themeJson.has("id"))
            assertFalse(themeJson.has("label"))
            if (preset.id != "field") {
                assertFalse(command.toString().contains("\"${preset.id}\""))
            }
        }
    }

    @Test
    fun `normalized serialization drops extras and restores fixed accent order`() {
        val normalized = BadgeTheme(
            version = 5,
            palette = "neon",
            background = "scanline",
            brightness = 90,
            accents = linkedMapOf(
                "clear" to 0xBFE9,
                "ignored" to 123,
                "drone" to 0xCFE5,
                "meta" to 0xFA75,
                "tracker" to 0x99DA,
                "flock" to 0xA357,
                "wifi_attack" to 0x373F,
            ),
        ).normalizedV1()

        val json = normalized.toJsonObject()
        assertEquals(1, json.get("version").asInt)
        assertEquals("neon", json.get("palette").asString)
        assertEquals(
            BadgeThemeAccentClasses.map { it.key },
            json.getAsJsonObject("accents").entrySet().map { it.key },
        )
        assertFalse(json.getAsJsonObject("accents").has("ignored"))
    }
}
