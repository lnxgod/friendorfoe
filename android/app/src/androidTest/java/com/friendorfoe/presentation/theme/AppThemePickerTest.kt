package com.friendorfoe.presentation.theme

import android.content.Context
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ApplicationProvider
import com.friendorfoe.data.preferences.AppThemeMode
import com.friendorfoe.data.preferences.AppThemePreferences
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class AppThemePickerTest {
    @get:Rule val compose = createComposeRule()

    @Test
    fun allChoicesUpdateTheRenderedPaletteAndSurvivePreferenceRecreation() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val store = AppThemePreferences(context)
        val original = store.mode.value
        val deviceDark = mutableStateOf(false)
        try {
            store.select(AppThemeMode.SYSTEM)
            compose.setContent {
                val mode by store.mode.collectAsState()
                FriendOrFoeTheme(darkTheme = mode.isDark(deviceDark.value)) {
                    CompositionLocalProvider(LocalAppThemeSelection provides AppThemeSelection(mode, store::select)) {
                        Column {
                            AppThemePicker()
                            Text(if (MaterialTheme.colorScheme.surface.luminance() < 0.5f) "Dark colors" else "Light colors")
                        }
                    }
                }
            }
            compose.onNodeWithText("Light colors").assertIsDisplayed()
            choose(AppThemeMode.SYSTEM, AppThemeMode.DARK)
            compose.onNodeWithText("Dark colors").assertIsDisplayed()
            assertEquals(AppThemeMode.DARK, AppThemePreferences(context).mode.value)
            compose.runOnIdle { deviceDark.value = true }
            choose(AppThemeMode.DARK, AppThemeMode.LIGHT)
            compose.onNodeWithText("Light colors").assertIsDisplayed()
            assertEquals(AppThemeMode.LIGHT, AppThemePreferences(context).mode.value)
            choose(AppThemeMode.LIGHT, AppThemeMode.SYSTEM)
            compose.onNodeWithText("Dark colors").assertIsDisplayed()
            compose.runOnIdle { deviceDark.value = false }
            compose.onNodeWithText("Light colors").assertIsDisplayed()
            assertEquals(AppThemeMode.SYSTEM, AppThemePreferences(context).mode.value)
        } finally {
            store.select(original)
        }
    }

    private fun choose(previous: AppThemeMode, next: AppThemeMode) {
        compose.onNodeWithTag("app_theme_picker").performClick()
        compose.onNodeWithTag("app_theme_${previous.storedValue}").assertIsSelected()
        compose.onNodeWithTag("app_theme_${next.storedValue}").performClick()
    }
}
