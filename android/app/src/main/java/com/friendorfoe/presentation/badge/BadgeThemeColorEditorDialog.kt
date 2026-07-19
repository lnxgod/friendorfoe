package com.friendorfoe.presentation.badge

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeThemeColorCodec
import com.friendorfoe.data.badge.Rgb888
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun BadgeThemeColorEditorDialog(
    accentLabel: String,
    initialRgb565: Int,
    onDismiss: () -> Unit,
    onSave: (Rgb888) -> Unit,
) {
    val initial = remember(initialRgb565) {
        BadgeThemeColorCodec.rgb565ToRgb888(initialRgb565.coerceIn(0, 0xFFFF))
    }
    var red by remember(initialRgb565) { mutableIntStateOf(initial.red) }
    var green by remember(initialRgb565) { mutableIntStateOf(initial.green) }
    var blue by remember(initialRgb565) { mutableIntStateOf(initial.blue) }
    var hexInput by remember(initialRgb565) {
        mutableStateOf(BadgeThemeColorCodec.effectiveHex(initialRgb565.coerceIn(0, 0xFFFF)))
    }

    val parsedHex = BadgeThemeColorCodec.parseHex(hexInput)
    val previewRgb = parsedHex ?: Rgb888(red, green, blue)
    val effectiveRgb565 = BadgeThemeColorCodec.rgb888ToRgb565(previewRgb)
    val effectiveHex = BadgeThemeColorCodec.effectiveHex(effectiveRgb565)

    fun setRgb(next: Rgb888) {
        red = next.red
        green = next.green
        blue = next.blue
        hexInput = rgb888Hex(next)
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Column {
                Text(text = accentLabel)
                Text(
                    text = "Signal color",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Surface(
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(10.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
                ) {
                    Row(
                        modifier = Modifier.padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Box(
                            modifier = Modifier
                                .size(58.dp)
                                .background(rgb565Color(effectiveRgb565), RoundedCornerShape(8.dp))
                                .testTag("badge_theme_color_preview")
                                .semantics {
                                    contentDescription = "$accentLabel effective color $effectiveHex"
                                },
                        )
                        Spacer(modifier = Modifier.width(12.dp))
                        Column {
                            Text(
                                text = "Effective on badge",
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                text = effectiveHex,
                                style = MaterialTheme.typography.titleMedium,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold,
                            )
                            Text(
                                text = String.format(Locale.ROOT, "RGB565 0x%04X", effectiveRgb565),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                fontFamily = FontFamily.Monospace,
                            )
                        }
                    }
                }

                OutlinedTextField(
                    value = hexInput,
                    onValueChange = { value ->
                        hexInput = value.uppercase(Locale.ROOT)
                        BadgeThemeColorCodec.parseHex(value)?.let { parsed ->
                            red = parsed.red
                            green = parsed.green
                            blue = parsed.blue
                        }
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .testTag("badge_theme_color_hex"),
                    label = { Text("Hex color") },
                    supportingText = {
                        Text(
                            if (parsedHex == null) {
                                "Enter exactly six hexadecimal digits"
                            } else {
                                "Input is quantized to the badge's RGB565 display"
                            },
                        )
                    },
                    isError = parsedHex == null,
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
                    textStyle = MaterialTheme.typography.bodyLarge.copy(fontFamily = FontFamily.Monospace),
                )

                RgbChannelControl(
                    label = "R",
                    value = red,
                    color = Color(0xFFE53935),
                    testTag = "badge_theme_color_red",
                    onValueChange = { setRgb(Rgb888(it, green, blue)) },
                )
                RgbChannelControl(
                    label = "G",
                    value = green,
                    color = Color(0xFF43A047),
                    testTag = "badge_theme_color_green",
                    onValueChange = { setRgb(Rgb888(red, it, blue)) },
                )
                RgbChannelControl(
                    label = "B",
                    value = blue,
                    color = Color(0xFF1E88E5),
                    testTag = "badge_theme_color_blue",
                    onValueChange = { setRgb(Rgb888(red, green, it)) },
                )
            }
        },
        confirmButton = {
            Button(
                onClick = { parsedHex?.let(onSave) },
                enabled = parsedHex != null,
                modifier = Modifier.testTag("badge_theme_color_save"),
            ) {
                Text("Save Color")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("Cancel")
            }
        },
    )
}

@Composable
private fun RgbChannelControl(
    label: String,
    value: Int,
    color: Color,
    testTag: String,
    onValueChange: (Int) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Black,
            color = color,
            modifier = Modifier.width(24.dp),
        )
        Slider(
            value = value.toFloat(),
            onValueChange = { onValueChange(it.roundToInt().coerceIn(0, 255)) },
            valueRange = 0f..255f,
            steps = 254,
            modifier = Modifier
                .weight(1f)
                .testTag(testTag)
                .semantics { stateDescription = "$label $value out of 255" },
        )
        Text(
            text = value.toString(),
            style = MaterialTheme.typography.labelMedium,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(34.dp),
        )
    }
}

private fun rgb888Hex(rgb: Rgb888): String = String.format(
    Locale.ROOT,
    "#%02X%02X%02X",
    rgb.red,
    rgb.green,
    rgb.blue,
)
