package com.friendorfoe.presentation.badge

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThemeAccentClasses
import com.friendorfoe.data.badge.BadgeThemeBackgrounds
import com.friendorfoe.data.badge.BadgeThemePalettes

private data class ThemeSwatch(val label: String, val rgb565: Int)

private val ThemeSwatches = listOf(
    ThemeSwatch("Ice", 0x07FF),
    ThemeSwatch("Gold", 0xFEA0),
    ThemeSwatch("Fire", 0xF800),
    ThemeSwatch("Rose", 0xF833),
    ThemeSwatch("Violet", 0xA81F),
    ThemeSwatch("Green", 0x2F65)
)

@Composable
fun BadgeAppearanceSection(
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    theme: BadgeTheme,
    themeHash: Long,
    onThemeChange: (BadgeTheme) -> Unit,
    onApply: () -> Unit,
    onReset: () -> Unit,
    onRefresh: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                RoundedCornerShape(8.dp)
            )
            .padding(8.dp)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .background(Color.Transparent)
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Badge Appearance",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "${theme.palette} / ${theme.background} / ${theme.brightness}%  #$themeHash",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            OutlinedButton(onClick = { onExpandedChange(!expanded) }) {
                Text(if (expanded) "Hide" else "Edit")
            }
        }

        if (!expanded) return@Column

        Spacer(modifier = Modifier.height(8.dp))
        Text("Palette", style = MaterialTheme.typography.labelSmall)
        SegmentedTextRow(
            values = BadgeThemePalettes,
            selected = theme.palette,
            onSelect = { onThemeChange(theme.copy(palette = it)) }
        )
        Spacer(modifier = Modifier.height(6.dp))
        Text("Background", style = MaterialTheme.typography.labelSmall)
        SegmentedTextRow(
            values = BadgeThemeBackgrounds,
            selected = theme.background,
            onSelect = { onThemeChange(theme.copy(background = it)) }
        )
        Spacer(modifier = Modifier.height(6.dp))
        Text(
            text = "Brightness ${theme.brightness}%",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Slider(
            value = theme.brightness.toFloat(),
            onValueChange = { onThemeChange(theme.copy(brightness = it.toInt().coerceIn(25, 100))) },
            valueRange = 25f..100f
        )
        BadgeThemeAccentClasses.forEach { accent ->
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    text = accent.label,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.weight(1f)
                )
                ThemeSwatches.forEach { swatch ->
                    val selected = theme.accents[accent.key] == swatch.rgb565
                    Box(
                        modifier = Modifier
                            .size(if (selected) 24.dp else 20.dp)
                            .background(rgb565Color(swatch.rgb565), RoundedCornerShape(4.dp))
                            .clickable {
                                onThemeChange(
                                    theme.copy(
                                        accents = theme.accents + (accent.key to swatch.rgb565)
                                    )
                                )
                            }
                    )
                    Spacer(modifier = Modifier.width(5.dp))
                }
            }
            Spacer(modifier = Modifier.height(5.dp))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Button(onClick = onApply) {
                Text("Apply")
            }
            OutlinedButton(onClick = onReset) {
                Text("Reset")
            }
            OutlinedButton(onClick = onRefresh) {
                Text("Refresh")
            }
        }
    }
}

@Composable
private fun SegmentedTextRow(
    values: List<String>,
    selected: String,
    onSelect: (String) -> Unit
) {
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        values.forEach { value ->
            OutlinedButton(
                onClick = { onSelect(value) },
                modifier = Modifier.weight(1f)
            ) {
                Text(
                    text = value.uppercase(),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (selected == value) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurface
                    },
                    maxLines = 1
                )
            }
        }
    }
}

private fun rgb565Color(rgb565: Int): Color {
    val r = ((rgb565 shr 11) and 0x1f) * 255 / 31
    val g = ((rgb565 shr 5) and 0x3f) * 255 / 63
    val b = (rgb565 and 0x1f) * 255 / 31
    return Color(r, g, b)
}
