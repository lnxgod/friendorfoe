package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeThemeContractTest {

    @Test
    fun defaultsAndHashMatchFirmware() {
        val theme = BadgeTheme.firmwareDefaults()

        assertEquals(
            mapOf(
                "drone" to 0xFEA0,
                "meta" to 0xF833,
                "tracker" to 0xF81F,
                "flock" to 0xA81F,
                "wifi_attack" to 0x07FF,
                "clear" to 0x2F65
            ),
            theme.accents
        )
        assertEquals(0xC3AA2A8DL, theme.firmwareHash())
    }

    @Test
    fun outgoingThemeUsesUnsignedDecimalAndPreservesPalette() {
        val theme = BadgeTheme.firmwareDefaults().copy(palette = "night")

        val json = theme.toJsonObject()

        assertEquals("night", json["palette"].asString)
        assertEquals(65184, json["accents"].asJsonObject["drone"].asInt)
        assertEquals(100, json["brightness"].asInt)
    }

    @Test
    fun zeroAccentAndUnknownPaletteAreRejected() {
        val zeroAccent = BadgeTheme.firmwareDefaults().copy(
            accents = BadgeTheme.firmwareDefaults().accents + ("drone" to 0)
        )
        val unknownPalette = BadgeTheme.firmwareDefaults().copy(palette = "future")

        assertTrue(BadgeTheme.validate(zeroAccent).isFailure)
        assertTrue(BadgeTheme.validate(unknownPalette).isFailure)
    }

    @Test
    fun hashAndSerializationUseCanonicalAccentOrder() {
        val defaults = BadgeTheme.firmwareDefaults()
        val reordered = defaults.copy(
            accents = defaults.accents.entries.reversed().associate { it.toPair() }
        )

        assertEquals(0xC3AA2A8DL, reordered.firmwareHash())
        assertEquals(
            BadgeTheme.accentOrder,
            reordered.toJsonObject()["accents"].asJsonObject.keySet().toList()
        )
    }
}
