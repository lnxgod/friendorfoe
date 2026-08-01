package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeThemePresetsTest {

    @Test
    fun `catalog order labels wire palettes and accent keys are frozen`() {
        assertEquals(
            listOf("field", "blacklight", "inferno", "ghostline", "obsidian_gold"),
            BadgeThemePresets.map { it.id },
        )
        assertEquals(
            listOf("Field", "Blacklight", "Inferno", "Ghostline", "Obsidian Gold"),
            BadgeThemePresets.map { it.label },
        )
        assertTrue(BadgeThemePresets.all { it.theme.palette in BadgeThemePalettes })

        val expectedKeys = listOf("drone", "meta", "tracker", "flock", "wifi_attack", "clear")
        assertEquals(expectedKeys, BadgeThemeAccentClasses.map { it.key })
        BadgeThemePresets.forEach { preset ->
            assertEquals(expectedKeys, preset.theme.accents.keys.toList())
        }
    }

    @Test
    fun `all five exact preset payloads are stable`() {
        val expected = listOf(
            ExpectedPreset(
                id = "field",
                palette = "field",
                background = "dark",
                brightness = 100,
                accents = listOf(0xFEA0, 0xF833, 0xF81F, 0xA81F, 0x07FF, 0x2F65),
            ),
            ExpectedPreset(
                id = "blacklight",
                palette = "neon",
                background = "scanline",
                brightness = 100,
                accents = listOf(0xCFE5, 0xFA75, 0x99DA, 0xA357, 0x373F, 0xBFE9),
            ),
            ExpectedPreset(
                id = "inferno",
                palette = "night",
                background = "dark",
                brightness = 100,
                accents = listOf(0xFD83, 0xF9AB, 0xFA44, 0xC349, 0x3EFE, 0x7FEE),
            ),
            ExpectedPreset(
                id = "ghostline",
                palette = "mono",
                background = "scanline",
                brightness = 100,
                accents = listOf(0xD7EA, 0x57B5, 0x26AF, 0x554F, 0x37FB, 0xAFEC),
            ),
            ExpectedPreset(
                id = "obsidian_gold",
                palette = "night",
                background = "dim",
                brightness = 90,
                accents = listOf(0xFE89, 0xFA73, 0xBC65, 0xAC8C, 0x7EDF, 0xD7EC),
            ),
        )

        expected.forEach { expectedPreset ->
            val theme = badgeThemePresetById(expectedPreset.id)!!.theme
            assertEquals(1, theme.version)
            assertEquals(expectedPreset.palette, theme.palette)
            assertEquals(expectedPreset.background, theme.background)
            assertEquals(expectedPreset.brightness, theme.brightness)
            assertEquals(
                expectedPreset.accents,
                BadgeThemeAccentClasses.map { theme.accents.getValue(it.key) },
            )
        }
    }

    @Test
    fun `preset lookup is exact and unknown ids return null`() {
        assertEquals("blacklight", badgeThemePresetById("blacklight")?.id)
        assertNull(badgeThemePresetById("Blacklight"))
        assertNull(badgeThemePresetById("unknown"))
    }

    @Test
    fun `normalization produces a bounded complete version one payload in fixed order`() {
        val normalized = BadgeTheme(
            version = 7,
            palette = "blacklight",
            background = "glow",
            brightness = 101,
            accents = linkedMapOf(
                "extra" to 123,
                "clear" to 0x1_0000,
                "tracker" to -1,
            ),
        ).normalizedV1()

        assertEquals(1, normalized.version)
        assertEquals("field", normalized.palette)
        assertEquals("dark", normalized.background)
        assertEquals(100, normalized.brightness)
        assertEquals(
            listOf("drone", "meta", "tracker", "flock", "wifi_attack", "clear"),
            normalized.accents.keys.toList(),
        )
        assertEquals(
            listOf(0xFEA0, 0xF833, 0, 0xA81F, 0x07FF, 0xFFFF),
            BadgeThemeAccentClasses.map { normalized.accents.getValue(it.key) },
        )
        assertEquals(25, normalized.copy(brightness = 0).normalizedV1().brightness)
    }

    @Test
    fun `recognition accepts normalized complete preset payloads`() {
        BadgeThemePresets.forEach { preset ->
            assertEquals(preset.id, recognizeBadgeThemePreset(preset.theme)?.id)
        }

        val blacklight = badgeThemePresetById("blacklight")!!.theme
        val reordered = blacklight.copy(
            version = 99,
            accents = linkedMapOf<String, Int>().apply {
                put("ignored", 0x1234)
                blacklight.accents.entries.reversed().forEach { (key, value) -> put(key, value) }
            },
        )
        assertEquals("blacklight", recognizeBadgeThemePreset(reordered)?.id)
    }

    @Test
    fun `recognition rejects every one field mismatch`() {
        val blacklight = badgeThemePresetById("blacklight")!!.theme
        val mismatches = buildList {
            add(blacklight.copy(palette = "night"))
            add(blacklight.copy(background = "dark"))
            add(blacklight.copy(brightness = 99))
            BadgeThemeAccentClasses.forEach { accent ->
                add(
                    blacklight.copy(
                        accents = blacklight.accents +
                            (accent.key to (blacklight.accents.getValue(accent.key) xor 0x0001)),
                    ),
                )
            }
        }

        mismatches.forEach { mismatch ->
            assertNull(recognizeBadgeThemePreset(mismatch))
        }
    }

    @Test
    fun `payload fingerprint is stable and uses fixed accent order`() {
        val blacklight = badgeThemePresetById("blacklight")!!.theme

        assertEquals(
            "1|neon|scanline|100|drone=53221|meta=64117|tracker=39386|" +
                "flock=41815|wifi_attack=14143|clear=49129",
            blacklight.payloadFingerprint(),
        )
    }

    private data class ExpectedPreset(
        val id: String,
        val palette: String,
        val background: String,
        val brightness: Int,
        val accents: List<Int>,
    )
}
