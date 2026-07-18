package com.friendorfoe.data.badge

data class BadgeThemePreset(
    val id: String,
    val label: String,
    val theme: BadgeTheme,
)

val BadgeThemePresets: List<BadgeThemePreset> = listOf(
    BadgeThemePreset(
        id = "field",
        label = "Field",
        theme = presetTheme(
            palette = "field",
            background = "dark",
            brightness = 100,
            0xFEA0, 0xF833, 0xF81F, 0xA81F, 0x07FF, 0x2F65,
        ),
    ),
    BadgeThemePreset(
        id = "blacklight",
        label = "Blacklight",
        theme = presetTheme(
            palette = "neon",
            background = "scanline",
            brightness = 100,
            0xCFE5, 0xFA75, 0x99DA, 0xA357, 0x373F, 0xBFE9,
        ),
    ),
    BadgeThemePreset(
        id = "inferno",
        label = "Inferno",
        theme = presetTheme(
            palette = "night",
            background = "dark",
            brightness = 100,
            0xFD83, 0xF9AB, 0xFA44, 0xC349, 0x3EFE, 0x7FEE,
        ),
    ),
    BadgeThemePreset(
        id = "ghostline",
        label = "Ghostline",
        theme = presetTheme(
            palette = "mono",
            background = "scanline",
            brightness = 100,
            0xD7EA, 0x57B5, 0x26AF, 0x554F, 0x37FB, 0xAFEC,
        ),
    ),
    BadgeThemePreset(
        id = "obsidian_gold",
        label = "Obsidian Gold",
        theme = presetTheme(
            palette = "night",
            background = "dim",
            brightness = 90,
            0xFE89, 0xFA73, 0xBC65, 0xAC8C, 0x7EDF, 0xD7EC,
        ),
    ),
)

fun badgeThemePresetById(id: String): BadgeThemePreset? =
    BadgeThemePresets.firstOrNull { it.id == id }

fun recognizeBadgeThemePreset(theme: BadgeTheme): BadgeThemePreset? {
    val fingerprint = theme.payloadFingerprint()
    return BadgeThemePresets.firstOrNull { it.theme.payloadFingerprint() == fingerprint }
}

fun BadgeTheme.normalizedV1(): BadgeTheme {
    val defaults = defaultBadgeTheme()
    return BadgeTheme(
        version = 1,
        palette = palette.takeIf { it in BadgeThemePalettes } ?: defaults.palette,
        background = background.takeIf { it in BadgeThemeBackgrounds } ?: defaults.background,
        brightness = brightness.coerceIn(25, 100),
        accents = BadgeThemeAccentClasses.associate { accent ->
            accent.key to (accents[accent.key] ?: accent.defaultRgb565).coerceIn(0, 0xFFFF)
        },
    )
}

internal fun BadgeTheme.payloadFingerprint(): String = buildString {
    val normalized = normalizedV1()
    append(normalized.version).append('|')
    append(normalized.palette).append('|')
    append(normalized.background).append('|')
    append(normalized.brightness)
    BadgeThemeAccentClasses.forEach { accent ->
        append('|')
            .append(accent.key)
            .append('=')
            .append(normalized.accents.getValue(accent.key))
    }
}

private fun presetTheme(
    palette: String,
    background: String,
    brightness: Int,
    vararg accents: Int,
): BadgeTheme {
    require(accents.size == BadgeThemeAccentClasses.size)
    return BadgeTheme(
        version = 1,
        palette = palette,
        background = background,
        brightness = brightness,
        accents = BadgeThemeAccentClasses.mapIndexed { index, accent ->
            accent.key to accents[index]
        }.toMap(linkedMapOf()),
    )
}
