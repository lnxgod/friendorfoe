package com.friendorfoe.presentation.badge

import androidx.compose.ui.graphics.Color

data class BadgeThemeAccentInfo(
    val firmwareKey: String,
    val label: String,
)

data class BadgeThemeSwatch(
    val label: String,
    val rgb565: Int,
)

data class BadgeThemeBackgroundOption(
    val label: String,
    val firmwareKey: String,
    val rgb565: Int,
)

val BadgeThemeAccentOptions = listOf(
    BadgeThemeAccentInfo("drone", "Drone"),
    BadgeThemeAccentInfo("meta", "Meta"),
    BadgeThemeAccentInfo("tracker", "Tracker"),
    BadgeThemeAccentInfo("flock", "Flock"),
    BadgeThemeAccentInfo("wifi_attack", "Wi-Fi Attack"),
    BadgeThemeAccentInfo("clear", "Clear"),
)

val SafeThemeSwatches = listOf(
    BadgeThemeSwatch("Ice", 0x07FF),
    BadgeThemeSwatch("Gold", 0xFEA0),
    BadgeThemeSwatch("Fire", 0xF800),
    BadgeThemeSwatch("Rose", 0xF833),
    BadgeThemeSwatch("Violet", 0xA81F),
    BadgeThemeSwatch("Green", 0x2F65),
)

val BadgeThemeBackgroundOptions = listOf(
    BadgeThemeBackgroundOption("Black", "dark", 0x0000),
    BadgeThemeBackgroundOption("Dim", "dim", 0x1082),
    BadgeThemeBackgroundOption("Blue-black", "scanline", 0x0108),
)

fun rgb565Color(rgb565: Int): Color {
    val red = ((rgb565 shr 11) and 0x1F) * 255 / 31
    val green = ((rgb565 shr 5) and 0x3F) * 255 / 63
    val blue = (rgb565 and 0x1F) * 255 / 31
    return Color(red, green, blue)
}

fun rgb565Code(rgb565: Int): String = "0x%04X · %d".format(rgb565, rgb565)

fun hashCode32(hash: Long?): String = hash?.let { "0x%08X".format(it and 0xFFFF_FFFFL) } ?: "Unavailable"
