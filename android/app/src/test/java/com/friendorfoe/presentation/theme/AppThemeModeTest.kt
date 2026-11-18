package com.friendorfoe.presentation.theme

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import com.friendorfoe.data.preferences.AppThemeMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AppThemeModeTest {
    @Test
    fun missingOrUnknownValueFollowsDeviceAndExplicitModesOverrideIt() {
        assertEquals(AppThemeMode.SYSTEM, AppThemeMode.fromStored(null))
        assertEquals(AppThemeMode.SYSTEM, AppThemeMode.fromStored("future-theme"))
        AppThemeMode.entries.forEach { mode -> assertEquals(mode, AppThemeMode.fromStored(mode.storedValue)) }
        for (systemDark in listOf(false, true)) {
            assertEquals(systemDark, AppThemeMode.SYSTEM.isDark(systemDark))
            assertEquals(false, AppThemeMode.LIGHT.isDark(systemDark))
            assertEquals(true, AppThemeMode.DARK.isDark(systemDark))
        }
    }

    @Test
    fun bodyLabelsAndActionTextHaveAccessibleContrastInBothPalettes() {
        for (dark in listOf(false, true)) {
            val scheme = appColorScheme(dark)
            val pairs = with(scheme) {
                listOf(
                    onSurface to surface, onBackground to background,
                    onSurfaceVariant to surfaceVariant, onSurfaceVariant to surfaceContainerHighest,
                    onPrimary to primary, onPrimaryContainer to primaryContainer,
                    onSecondary to secondary, onSecondaryContainer to secondaryContainer,
                    onTertiary to tertiary, onTertiaryContainer to tertiaryContainer,
                    onError to error, onErrorContainer to errorContainer,
                    inverseOnSurface to inverseSurface,
                )
            }
            pairs.forEach { (foreground, background) ->
                assertTrue("Insufficient text contrast for $foreground on $background", contrast(foreground, background) >= 4.5)
            }
        }
    }

    private fun contrast(first: Color, second: Color): Double {
        val a = first.luminance().toDouble()
        val b = second.luminance().toDouble()
        return (maxOf(a, b) + 0.05) / (minOf(a, b) + 0.05)
    }
}
