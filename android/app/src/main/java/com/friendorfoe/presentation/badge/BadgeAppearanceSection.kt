package com.friendorfoe.presentation.badge

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThemeAccentClasses
import com.friendorfoe.data.badge.BadgeThemeAccentInfo
import com.friendorfoe.data.badge.BadgeThemeBackgrounds
import com.friendorfoe.data.badge.BadgeThemeColorCodec
import com.friendorfoe.data.badge.BadgeThemePalettes
import com.friendorfoe.data.badge.BadgeThemePreset
import com.friendorfoe.data.badge.BadgeThemePresets
import com.friendorfoe.data.badge.BadgeThemeProfile
import com.friendorfoe.data.badge.Rgb888
import com.friendorfoe.data.badge.defaultBadgeTheme
import com.friendorfoe.data.badge.normalizedV1
import com.friendorfoe.data.badge.recognizeBadgeThemePreset

internal const val BadgeThemeStudioExpandedMaxHeightDp = 480

internal enum class BadgeThemeStudioExpandedRegion {
    ScrollableContent,
    StickyActions,
}

internal val BadgeThemeStudioExpandedRegions = listOf(
    BadgeThemeStudioExpandedRegion.ScrollableContent,
    BadgeThemeStudioExpandedRegion.StickyActions,
)

internal sealed interface BadgeThemeProfileResolution {
    data class Found(val profile: BadgeThemeProfile) : BadgeThemeProfileResolution
    data object Missing : BadgeThemeProfileResolution
}

internal fun resolveBadgeThemeProfile(
    profiles: List<BadgeThemeProfile>,
    profileId: String,
): BadgeThemeProfileResolution = profiles
    .firstOrNull { it.id == profileId }
    ?.let(BadgeThemeProfileResolution::Found)
    ?: BadgeThemeProfileResolution.Missing

internal const val BadgeThemeProfileMissingMessage = "Profile no longer exists."

internal enum class BadgeThemeProfileMutation(val failureVerb: String) {
    Rename("renamed"),
    Replace("replaced"),
    Delete("deleted"),
}

internal fun badgeThemeProfileMutationFailureMessage(
    mutation: BadgeThemeProfileMutation,
): String = "Profile could not be ${mutation.failureVerb}. " +
    "It may have changed or no longer exists."

internal data class BadgeThemeSelectedOptionSemantics(
    val selected: Boolean,
    val stateDescription: String,
)

internal fun badgeThemeSelectedOptionSemantics(
    value: String,
    selectedValue: String,
): BadgeThemeSelectedOptionSemantics {
    val isSelected = value == selectedValue
    return BadgeThemeSelectedOptionSemantics(
        selected = isSelected,
        stateDescription = if (isSelected) "Selected" else "Not selected",
    )
}

internal sealed interface BadgeThemeStudioAction {
    data class SelectDraft(val theme: BadgeTheme) : BadgeThemeStudioAction
    data class SetAccent(val key: String, val color: Rgb888) : BadgeThemeStudioAction
    data object ResetDraft : BadgeThemeStudioAction
    data object Apply : BadgeThemeStudioAction
    data object Refresh : BadgeThemeStudioAction
}

internal enum class BadgeThemeStudioCommand {
    None,
    Apply,
    Refresh,
}

internal data class BadgeThemeStudioTransition(
    val draft: BadgeTheme,
    val command: BadgeThemeStudioCommand = BadgeThemeStudioCommand.None,
)

internal fun reduceBadgeThemeStudio(
    draft: BadgeTheme,
    action: BadgeThemeStudioAction,
): BadgeThemeStudioTransition = when (action) {
    is BadgeThemeStudioAction.SelectDraft -> BadgeThemeStudioTransition(
        action.theme.normalizedV1(),
    )
    is BadgeThemeStudioAction.SetAccent -> BadgeThemeStudioTransition(
        draft.copy(
            accents = draft.accents +
                (action.key to BadgeThemeColorCodec.rgb888ToRgb565(action.color)),
        ).normalizedV1(),
    )
    BadgeThemeStudioAction.ResetDraft -> BadgeThemeStudioTransition(defaultBadgeTheme())
    BadgeThemeStudioAction.Apply -> BadgeThemeStudioTransition(
        draft = draft.normalizedV1(),
        command = BadgeThemeStudioCommand.Apply,
    )
    BadgeThemeStudioAction.Refresh -> BadgeThemeStudioTransition(
        draft = draft,
        command = BadgeThemeStudioCommand.Refresh,
    )
}

private sealed interface ProfileNameDialogMode {
    data object Create : ProfileNameDialogMode
    data class Rename(val profileId: String) : ProfileNameDialogMode
}

@Composable
fun BadgeAppearanceSection(
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    theme: BadgeTheme,
    themeHash: Long,
    profiles: List<BadgeThemeProfile>,
    onThemeChange: (BadgeTheme) -> Unit,
    onCreateProfile: (String, BadgeTheme) -> Boolean,
    onRenameProfile: (String, String) -> Boolean,
    onReplaceProfile: (String, BadgeTheme) -> Boolean,
    onDeleteProfile: (String) -> Boolean,
    onApply: (BadgeTheme) -> Unit,
    onRefresh: () -> Unit,
) {
    var colorEditorAccent by remember { mutableStateOf<BadgeThemeAccentInfo?>(null) }
    var profileNameDialog by remember { mutableStateOf<ProfileNameDialogMode?>(null) }
    var pendingReplaceProfileId by remember { mutableStateOf<String?>(null) }
    var pendingDeleteProfileId by remember { mutableStateOf<String?>(null) }
    var profileMessage by remember { mutableStateOf<String?>(null) }

    val renameProfileId = (profileNameDialog as? ProfileNameDialogMode.Rename)?.profileId
    val renameProfileResolution = renameProfileId?.let { profileId ->
        resolveBadgeThemeProfile(profiles, profileId)
    }
    val replaceProfileResolution = pendingReplaceProfileId?.let { profileId ->
        resolveBadgeThemeProfile(profiles, profileId)
    }
    val deleteProfileResolution = pendingDeleteProfileId?.let { profileId ->
        resolveBadgeThemeProfile(profiles, profileId)
    }

    LaunchedEffect(renameProfileId, renameProfileResolution) {
        if (
            renameProfileId != null &&
            renameProfileResolution == BadgeThemeProfileResolution.Missing
        ) {
            profileNameDialog = null
            profileMessage = BadgeThemeProfileMissingMessage
        }
    }
    LaunchedEffect(pendingReplaceProfileId, replaceProfileResolution) {
        if (
            pendingReplaceProfileId != null &&
            replaceProfileResolution == BadgeThemeProfileResolution.Missing
        ) {
            pendingReplaceProfileId = null
            profileMessage = BadgeThemeProfileMissingMessage
        }
    }
    LaunchedEffect(pendingDeleteProfileId, deleteProfileResolution) {
        if (
            pendingDeleteProfileId != null &&
            deleteProfileResolution == BadgeThemeProfileResolution.Missing
        ) {
            pendingDeleteProfileId = null
            profileMessage = BadgeThemeProfileMissingMessage
        }
    }

    fun dispatch(action: BadgeThemeStudioAction) {
        val transition = reduceBadgeThemeStudio(theme, action)
        if (transition.draft != theme) onThemeChange(transition.draft)
        when (transition.command) {
            BadgeThemeStudioCommand.None -> Unit
            BadgeThemeStudioCommand.Apply -> onApply(transition.draft)
            BadgeThemeStudioCommand.Refresh -> onRefresh()
        }
    }

    val recognizedPreset = recognizeBadgeThemePreset(theme)
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                RoundedCornerShape(8.dp),
            )
            .testTag("badge_appearance_section")
            .padding(8.dp),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Badge Appearance",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    text = buildString {
                        append(recognizedPreset?.label ?: "Custom")
                        append("  •  ${theme.palette} / ${theme.background} / ${theme.brightness}%")
                        append("  #$themeHash")
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            OutlinedButton(
                onClick = { onExpandedChange(!expanded) },
                modifier = Modifier.testTag("badge_appearance_toggle"),
            ) {
                Text(if (expanded) "Hide" else "Edit")
            }
        }

        if (!expanded) return@Column

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(max = BadgeThemeStudioExpandedMaxHeightDp.dp)
                .testTag("badge_theme_studio_expanded")
                .semantics {
                    contentDescription = "Bounded badge palette studio with sticky actions"
                },
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .testTag("badge_theme_studio_scroll")
                    .semantics {
                        contentDescription = "Scrollable badge palette studio content"
                    },
            ) {
            Spacer(modifier = Modifier.height(12.dp))
            BadgeThemePreview(theme = theme)

            Spacer(modifier = Modifier.height(14.dp))
            Text(
                text = "Presets",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = "Load a complete palette into this draft. The badge stays unchanged until Apply.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            BadgeThemePresetStrip(
                selectedPresetId = recognizedPreset?.id,
                onSelect = { preset ->
                    profileMessage = null
                    dispatch(BadgeThemeStudioAction.SelectDraft(preset.theme))
                },
            )

            Spacer(modifier = Modifier.height(12.dp))
            BadgeThemeStudioGroup(
            title = "Interface",
            subtitle = "Compatible badge chrome, background treatment, and display brightness.",
        ) {
            Text(
                text = "Base palette",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            SegmentedTextRow(
                values = BadgeThemePalettes,
                selected = theme.palette,
                testTagPrefix = "badge_theme_palette",
                onSelect = { palette ->
                    dispatch(BadgeThemeStudioAction.SelectDraft(theme.copy(palette = palette)))
                },
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = "Background",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            SegmentedTextRow(
                values = BadgeThemeBackgrounds,
                selected = theme.background,
                testTagPrefix = "badge_theme_background",
                onSelect = { background ->
                    dispatch(BadgeThemeStudioAction.SelectDraft(theme.copy(background = background)))
                },
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "Brightness",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(modifier = Modifier.weight(1f))
                Text(
                    text = "${theme.brightness}%",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            Slider(
                value = theme.brightness.toFloat(),
                onValueChange = { value ->
                    dispatch(
                        BadgeThemeStudioAction.SelectDraft(
                            theme.copy(brightness = value.toInt().coerceIn(25, 100)),
                        ),
                    )
                },
                valueRange = 25f..100f,
                steps = 74,
                modifier = Modifier.testTag("badge_theme_brightness"),
            )
        }

            Spacer(modifier = Modifier.height(10.dp))
            BadgeThemeStudioGroup(
            title = "Signal Colors",
            subtitle = "Each value shows the effective quantized color rendered by the RGB565 badge display.",
        ) {
            BadgeThemeAccentClasses.forEachIndexed { index, accent ->
                val rgb565 = theme.accents[accent.key] ?: accent.defaultRgb565
                BadgeThemeAccentRow(
                    accent = accent,
                    rgb565 = rgb565,
                    onClick = { colorEditorAccent = accent },
                )
                if (index != BadgeThemeAccentClasses.lastIndex) {
                    HorizontalDivider(
                        modifier = Modifier.padding(vertical = 6.dp),
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
                    )
                }
            }
        }

            Spacer(modifier = Modifier.height(10.dp))
            BadgeThemeStudioGroup(
            title = "Saved Profiles",
            subtitle = "Profiles stay on this Android device and are shared by List and Privacy.",
        ) {
            Button(
                onClick = { profileNameDialog = ProfileNameDialogMode.Create },
                modifier = Modifier.testTag("badge_theme_profile_create"),
            ) {
                Text("Save New Profile")
            }
            if (profiles.isEmpty()) {
                Text(
                    text = "No saved profiles yet.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 10.dp, bottom = 2.dp),
                )
            } else {
                profiles.forEach { profile ->
                    HorizontalDivider(
                        modifier = Modifier.padding(top = 10.dp),
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
                    )
                    BadgeThemeProfileRow(
                        profile = profile,
                        onLoad = {
                            profileMessage = "Loaded ${profile.name} into the draft. Press Apply to update the badge."
                            dispatch(BadgeThemeStudioAction.SelectDraft(profile.theme))
                        },
                        onReplace = { pendingReplaceProfileId = profile.id },
                        onRename = {
                            profileNameDialog = ProfileNameDialogMode.Rename(profile.id)
                        },
                        onDelete = { pendingDeleteProfileId = profile.id },
                    )
                }
            }
            profileMessage?.let { message ->
                Text(
                    text = message,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .padding(top = 10.dp)
                        .testTag("badge_theme_profile_message"),
                )
            }
            }
        }

        Spacer(modifier = Modifier.height(12.dp))
        Surface(
            modifier = Modifier
                .fillMaxWidth()
                .testTag("badge_theme_studio_actions")
                .semantics {
                    contentDescription = "Sticky badge palette studio actions"
                },
            shape = RoundedCornerShape(8.dp),
            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        ) {
            Column(modifier = Modifier.padding(10.dp)) {
                Text(
                    text = "Draft actions",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState())
                        .padding(top = 6.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Button(
                        onClick = { dispatch(BadgeThemeStudioAction.Apply) },
                        modifier = Modifier.testTag("badge_theme_apply"),
                    ) {
                        Text("Apply")
                    }
                    OutlinedButton(
                        onClick = {
                            profileMessage = "Draft reset to Field. Saved profiles were not changed."
                            dispatch(BadgeThemeStudioAction.ResetDraft)
                        },
                        modifier = Modifier.testTag("badge_theme_reset"),
                    ) {
                        Text("Reset")
                    }
                    OutlinedButton(
                        onClick = { dispatch(BadgeThemeStudioAction.Refresh) },
                        modifier = Modifier.testTag("badge_theme_refresh"),
                    ) {
                        Text("Refresh")
                    }
                }
                Text(
                    text = "Only Apply writes this draft to the badge.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }
        }
        }
    }

    colorEditorAccent?.let { accent ->
        BadgeThemeColorEditorDialog(
            accentLabel = accent.label,
            initialRgb565 = theme.accents[accent.key] ?: accent.defaultRgb565,
            onDismiss = { colorEditorAccent = null },
            onSave = { rgb ->
                dispatch(BadgeThemeStudioAction.SetAccent(accent.key, rgb))
                colorEditorAccent = null
            },
        )
    }

    profileNameDialog?.let { mode ->
        val renamedProfile = (renameProfileResolution as? BadgeThemeProfileResolution.Found)?.profile
        if (mode == ProfileNameDialogMode.Create || renamedProfile != null) {
            BadgeThemeProfileNameDialog(
                renamedProfile = renamedProfile,
                profiles = profiles,
                onDismiss = { profileNameDialog = null },
                onCreate = { name ->
                    val saved = onCreateProfile(name, theme.normalizedV1())
                    profileMessage = if (saved) {
                        "Saved ${name.trim()}. The badge was not changed."
                    } else {
                        "Profile was not saved. Check the name and try again."
                    }
                    saved
                },
                onRename = { id, name ->
                    val renamed = onRenameProfile(id, name)
                    profileMessage = if (renamed) {
                        "Renamed profile to ${name.trim()}."
                    } else {
                        badgeThemeProfileMutationFailureMessage(BadgeThemeProfileMutation.Rename)
                    }
                    renamed
                },
            )
        }
    }

    (replaceProfileResolution as? BadgeThemeProfileResolution.Found)?.profile?.let { profile ->
        BadgeThemeProfileConfirmationDialog(
            title = "Replace ${profile.name}?",
            body = "This replaces the saved profile with the current draft. The badge stays unchanged.",
            confirmLabel = "Replace",
            testTag = "badge_theme_profile_replace_confirm",
            onDismiss = { pendingReplaceProfileId = null },
            onConfirm = {
                val replaced = onReplaceProfile(profile.id, theme.normalizedV1())
                profileMessage = if (replaced) {
                    "Replaced ${profile.name}. The badge was not changed."
                } else {
                    badgeThemeProfileMutationFailureMessage(BadgeThemeProfileMutation.Replace)
                }
                pendingReplaceProfileId = null
            },
        )
    }

    (deleteProfileResolution as? BadgeThemeProfileResolution.Found)?.profile?.let { profile ->
        BadgeThemeProfileConfirmationDialog(
            title = "Delete ${profile.name}?",
            body = "This removes the saved Android profile. It does not reset or write to the badge.",
            confirmLabel = "Delete",
            testTag = "badge_theme_profile_delete_confirm",
            onDismiss = { pendingDeleteProfileId = null },
            onConfirm = {
                val deleted = onDeleteProfile(profile.id)
                profileMessage = if (deleted) {
                    "Deleted ${profile.name}. The current draft was not changed."
                } else {
                    badgeThemeProfileMutationFailureMessage(BadgeThemeProfileMutation.Delete)
                }
                pendingDeleteProfileId = null
            },
        )
    }
}

@Composable
private fun BadgeThemePresetStrip(
    selectedPresetId: String?,
    onSelect: (BadgeThemePreset) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(top = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        BadgeThemePresets.forEach { preset ->
            val selected = preset.id == selectedPresetId
            Surface(
                modifier = Modifier
                    .width(132.dp)
                    .clickable { onSelect(preset) }
                    .testTag("badge_theme_preset_${preset.id}")
                    .semantics {
                        contentDescription =
                            "${preset.label} preset${if (selected) ", selected" else ""}"
                    },
                shape = RoundedCornerShape(9.dp),
                color = if (selected) {
                    MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.72f)
                } else {
                    MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
                },
                border = BorderStroke(
                    width = if (selected) 2.dp else 1.dp,
                    color = if (selected) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.outlineVariant
                    },
                ),
            ) {
                Column(modifier = Modifier.padding(10.dp)) {
                    Text(
                        text = preset.label,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = "${preset.theme.palette} • ${preset.theme.background}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                    Row(
                        modifier = Modifier.padding(top = 8.dp),
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        BadgeThemeAccentClasses.forEach { accent ->
                            val rgb565 = preset.theme.accents.getValue(accent.key)
                            Box(
                                modifier = Modifier
                                    .size(13.dp)
                                    .background(rgb565Color(rgb565), RoundedCornerShape(3.dp))
                                    .semantics {
                                        contentDescription =
                                            "${accent.label} ${BadgeThemeColorCodec.effectiveHex(rgb565)}"
                                    },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun BadgeThemeStudioGroup(
    title: String,
    subtitle: String,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(9.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.38f),
        tonalElevation = 0.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 2.dp, bottom = 10.dp),
            )
            content()
        }
    }
}

@Composable
private fun SegmentedTextRow(
    values: List<String>,
    selected: String,
    testTagPrefix: String,
    onSelect: (String) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        values.forEach { value ->
            val optionSemantics = badgeThemeSelectedOptionSemantics(value, selected)
            OutlinedButton(
                onClick = { onSelect(value) },
                modifier = Modifier
                    .weight(1f)
                    .testTag("${testTagPrefix}_$value")
                    .semantics {
                        this.selected = optionSemantics.selected
                        stateDescription = optionSemantics.stateDescription
                    },
                border = BorderStroke(
                    width = if (selected == value) 2.dp else 1.dp,
                    color = if (selected == value) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.outline
                    },
                ),
            ) {
                Text(
                    text = value.uppercase(),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (selected == value) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurface
                    },
                    maxLines = 1,
                )
            }
        }
    }
}

@Composable
private fun BadgeThemeAccentRow(
    accent: BadgeThemeAccentInfo,
    rgb565: Int,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .testTag("badge_theme_accent_${accent.key}")
            .semantics {
                contentDescription =
                    "Edit ${accent.label} color, ${BadgeThemeColorCodec.effectiveHex(rgb565)}"
            }
            .padding(vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .background(rgb565Color(rgb565), RoundedCornerShape(7.dp)),
        )
        Spacer(modifier = Modifier.width(10.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = accent.label,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = BadgeThemeColorCodec.effectiveHex(rgb565),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace,
            )
        }
        Text(
            text = "Edit",
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.SemiBold,
            color = MaterialTheme.colorScheme.primary,
        )
    }
}

@Composable
private fun BadgeThemeProfileRow(
    profile: BadgeThemeProfile,
    onLoad: () -> Unit,
    onReplace: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
) {
    val preset = recognizeBadgeThemePreset(profile.theme)
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 8.dp)
            .testTag("badge_theme_profile_${profile.id}"),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(28.dp)
                    .background(
                        rgb565Color(profile.theme.accents["drone"] ?: 0),
                        RoundedCornerShape(6.dp),
                    ),
            )
            Spacer(modifier = Modifier.width(9.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = profile.name,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = preset?.label ?: "Custom ${profile.theme.palette}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            TextButton(
                onClick = onLoad,
                modifier = Modifier.testTag("badge_theme_profile_load_${profile.id}"),
            ) {
                Text("Load")
            }
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            TextButton(
                onClick = onReplace,
                modifier = Modifier.testTag("badge_theme_profile_replace_${profile.id}"),
            ) { Text("Replace") }
            TextButton(
                onClick = onRename,
                modifier = Modifier.testTag("badge_theme_profile_rename_${profile.id}"),
            ) { Text("Rename") }
            TextButton(
                onClick = onDelete,
                modifier = Modifier.testTag("badge_theme_profile_delete_${profile.id}"),
            ) {
                Text("Delete", color = MaterialTheme.colorScheme.error)
            }
        }
    }
}

@Composable
private fun BadgeThemeProfileNameDialog(
    renamedProfile: BadgeThemeProfile?,
    profiles: List<BadgeThemeProfile>,
    onDismiss: () -> Unit,
    onCreate: (String) -> Boolean,
    onRename: (String, String) -> Boolean,
) {
    var name by remember(renamedProfile) { mutableStateOf(renamedProfile?.name.orEmpty()) }
    var saveError by remember(renamedProfile) { mutableStateOf<String?>(null) }
    val normalizedName = name.trim()
    val duplicate = profiles.any { profile ->
        profile.id != renamedProfile?.id && profile.name.equals(normalizedName, ignoreCase = true)
    }
    val changed = renamedProfile == null || normalizedName != renamedProfile.name
    val valid = normalizedName.length in 1..32 && !duplicate && changed
    val helper = when {
        normalizedName.isEmpty() -> "Name is required"
        normalizedName.length > 32 -> "Use 32 characters or fewer"
        duplicate -> "A profile with this name already exists"
        !changed -> "Enter a new name"
        saveError != null -> saveError!!
        else -> "Names are unique regardless of capitalization"
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(if (renamedProfile == null) "Save Profile" else "Rename Profile")
        },
        text = {
            OutlinedTextField(
                value = name,
                onValueChange = {
                    name = it
                    saveError = null
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("badge_theme_profile_name"),
                label = { Text("Profile name") },
                supportingText = { Text(helper) },
                isError = !valid && name.isNotEmpty(),
                singleLine = true,
            )
        },
        confirmButton = {
            Button(
                onClick = {
                    val saved = if (renamedProfile == null) {
                        onCreate(normalizedName)
                    } else {
                        onRename(renamedProfile.id, normalizedName)
                    }
                    if (saved) {
                        onDismiss()
                    } else {
                        saveError = if (renamedProfile == null) {
                            "The profile could not be saved"
                        } else {
                            badgeThemeProfileMutationFailureMessage(BadgeThemeProfileMutation.Rename)
                        }
                    }
                },
                enabled = valid,
                modifier = Modifier.testTag("badge_theme_profile_name_save"),
            ) {
                Text(if (renamedProfile == null) "Save" else "Rename")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

@Composable
private fun BadgeThemeProfileConfirmationDialog(
    title: String,
    body: String,
    confirmLabel: String,
    testTag: String,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(body) },
        confirmButton = {
            Button(
                onClick = onConfirm,
                modifier = Modifier.testTag(testTag),
            ) {
                Text(confirmLabel)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}
