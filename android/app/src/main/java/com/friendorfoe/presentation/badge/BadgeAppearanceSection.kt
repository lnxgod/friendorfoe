package com.friendorfoe.presentation.badge

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
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
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeTheme

@Composable
fun BadgeAppearanceSection(
    theme: BadgeTheme?,
    themeHash: Long?,
    enabled: Boolean,
    unavailableReason: String?,
    onThemeChange: ((BadgeTheme) -> BadgeTheme) -> Unit,
) {
    BadgeSectionCard {
        Text(
            "LCD accent colors",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Select badge-safe RGB565 values. The badge reports the exact stored code.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Theme hash ${hashCode32(themeHash)}",
            style = MaterialTheme.typography.labelMedium,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (theme == null) {
            Text(
                unavailableReason ?: "Theme readback is unavailable",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        } else {
            BadgeThemeAccentOptions.forEach { info ->
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
                )
                AccentRow(
                    info = info,
                    selectedRgb565 = theme.accents.getValue(info.firmwareKey),
                    enabled = enabled,
                    onSelect = { rgb565 ->
                        onThemeChange { current ->
                            current.copy(
                                accents = LinkedHashMap(current.accents).apply {
                                    put(info.firmwareKey, rgb565)
                                },
                            )
                        }
                    },
                )
            }

            HorizontalDivider(
                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
            )
            Text("Background", style = MaterialTheme.typography.labelLarge)
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .testTag("badge_backgrounds"),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                BadgeThemeBackgroundOptions.forEach { option ->
                    FilterChip(
                        selected = theme.background == option.firmwareKey,
                        onClick = {
                            onThemeChange { current ->
                                current.copy(background = option.firmwareKey)
                            }
                        },
                        enabled = enabled,
                        label = {
                            Text(
                                "${option.label} — ${option.firmwareKey} — 0x%04X".format(
                                    option.rgb565,
                                ),
                            )
                        },
                        modifier = Modifier
                            .heightIn(min = 48.dp)
                            .testTag("badge_background_${option.firmwareKey}"),
                    )
                }
            }

            Text(
                "Color intensity ${theme.intensity}%",
                style = MaterialTheme.typography.labelLarge,
            )
            Slider(
                value = theme.intensity.toFloat(),
                onValueChange = { value ->
                    onThemeChange { current ->
                        current.copy(intensity = value.toInt().coerceIn(25, 100))
                    }
                },
                enabled = enabled,
                valueRange = 25f..100f,
                steps = 74,
                modifier = Modifier
                    .fillMaxWidth()
                    .semantics { contentDescription = "Color intensity" }
                    .testTag("badge_intensity"),
            )
            Text(
                "25%–100%",
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "Color intensity changes RGB565 output; the badge backlight is fixed.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!enabled && unavailableReason != null) {
                Text(
                    unavailableReason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

@Composable
private fun AccentRow(
    info: BadgeThemeAccentInfo,
    selectedRgb565: Int,
    enabled: Boolean,
    onSelect: (Int) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp)
            .testTag("badge_accent_${info.firmwareKey}"),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(24.dp)
                    .background(rgb565Color(selectedRgb565), RoundedCornerShape(6.dp)),
            )
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(info.label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
                Text(
                    info.firmwareKey,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                rgb565Code(selectedRgb565),
                style = MaterialTheme.typography.labelMedium,
                fontFamily = FontFamily.Monospace,
            )
        }
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SafeThemeSwatches.forEach { swatch ->
                FilterChip(
                    selected = swatch.rgb565 == selectedRgb565,
                    onClick = { onSelect(swatch.rgb565) },
                    enabled = enabled,
                    leadingIcon = {
                        Box(
                            Modifier
                                .size(16.dp)
                                .background(rgb565Color(swatch.rgb565), RoundedCornerShape(4.dp)),
                        )
                    },
                    label = { Text(swatch.label) },
                    modifier = Modifier
                        .heightIn(min = 48.dp)
                        .semantics {
                            contentDescription =
                                "${info.label} ${swatch.label} color, 0x%04X".format(
                                    swatch.rgb565,
                                )
                        }
                        .testTag(
                            "badge_accent_${info.firmwareKey}_swatch_%04x".format(
                                swatch.rgb565,
                            ),
                        ),
                )
            }
        }
    }
}

@Composable
internal fun BadgeSectionCard(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f),
        ),
        content = {
            Column(
                Modifier.fillMaxWidth().padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
                content = content,
            )
        },
    )
}

@Composable
internal fun BadgeResponsiveActionPair(
    first: @Composable (Modifier) -> Unit,
    second: @Composable (Modifier) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val stackActions = maxWidth < 360.dp || LocalDensity.current.fontScale >= 1.3f
        if (stackActions) {
            Column(
                Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                first(Modifier.fillMaxWidth())
                second(Modifier.fillMaxWidth())
            }
        } else {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                first(Modifier.weight(1f))
                second(Modifier.weight(1f))
            }
        }
    }
}
