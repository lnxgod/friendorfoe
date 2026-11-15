package com.friendorfoe.presentation.theme

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.preferences.AppThemeMode
import com.friendorfoe.presentation.components.FofActionRow

internal data class AppThemeSelection(val mode: AppThemeMode, val onSelect: (AppThemeMode) -> Unit)

internal val LocalAppThemeSelection = staticCompositionLocalOf {
    AppThemeSelection(AppThemeMode.SYSTEM) {}
}

private val AppThemeMode.label: String
    get() = when (this) {
        AppThemeMode.SYSTEM -> "System"
        AppThemeMode.LIGHT -> "Light"
        AppThemeMode.DARK -> "Dark"
    }

@Composable
internal fun AppThemePicker() {
    val selection = LocalAppThemeSelection.current
    var open by rememberSaveable { mutableStateOf(false) }
    FofActionRow(
        title = "App theme",
        description = if (selection.mode == AppThemeMode.SYSTEM) "System · follows your device" else selection.mode.label,
        trailingLabel = "Change",
        modifier = Modifier.testTag("app_theme_picker"),
        onClick = { open = true },
    )
    if (open) {
        AlertDialog(
            onDismissRequest = { open = false },
            title = { Text("App theme") },
            text = {
                Column(Modifier.selectableGroup().verticalScroll(rememberScrollState())) {
                    AppThemeMode.entries.forEach { mode ->
                        val selected = mode == selection.mode
                        Row(
                            Modifier.fillMaxWidth().heightIn(min = 52.dp)
                                .selectable(selected = selected, role = Role.RadioButton) {
                                    open = false
                                    selection.onSelect(mode)
                                }.testTag("app_theme_${mode.storedValue}"),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = selected, onClick = null)
                            Spacer(Modifier.width(16.dp))
                            Text(mode.label)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { open = false }) { Text("Cancel") } },
        )
    }
}
