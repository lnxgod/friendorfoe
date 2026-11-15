package com.friendorfoe.data.preferences

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

enum class AppThemeMode(val storedValue: String) {
    SYSTEM("system"), LIGHT("light"), DARK("dark");

    fun isDark(systemDark: Boolean): Boolean = when (this) {
        SYSTEM -> systemDark
        LIGHT -> false
        DARK -> true
    }

    companion object {
        fun fromStored(value: String?): AppThemeMode =
            entries.firstOrNull { it.storedValue == value } ?: SYSTEM
    }
}

@Singleton
class AppThemePreferences @Inject constructor(@ApplicationContext context: Context) {
    // Read synchronously so the first Compose frame uses the saved appearance.
    private val prefs = context.getSharedPreferences("app_appearance", Context.MODE_PRIVATE)
    private val selection = MutableStateFlow(AppThemeMode.fromStored(prefs.getString("theme", null)))
    val mode = selection.asStateFlow()

    fun select(mode: AppThemeMode) {
        prefs.edit().putString("theme", mode.storedValue).apply()
        selection.value = mode
    }
}
