package com.friendorfoe.presentation.badge

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThemeAccentClasses
import com.friendorfoe.data.badge.BadgeThemeColorCodec
import com.friendorfoe.data.badge.normalizedV1

internal enum class BadgeThemePreviewLaneKind {
    Global,
    Ble,
    Wifi,
}

internal data class BadgeThemePreviewLane(
    val kind: BadgeThemePreviewLaneKind,
    val token: String,
    val title: String,
    val detail: String,
    val state: String,
    val accentKey: String,
    val testTag: String,
)

internal val BadgeThemePreviewLanes = listOf(
    BadgeThemePreviewLane(
        kind = BadgeThemePreviewLaneKind.Global,
        token = "01 GLOBAL",
        title = "REMOTE ID DRONE",
        detail = "NEAR  •  -54 dB",
        state = "LOCK",
        accentKey = "drone",
        testTag = "badge_theme_preview_global_1",
    ),
    BadgeThemePreviewLane(
        kind = BadgeThemePreviewLaneKind.Global,
        token = "02 GLOBAL",
        title = "META GLASSES",
        detail = "CLOSE  •  -61 dB",
        state = "WATCH",
        accentKey = "meta",
        testTag = "badge_theme_preview_global_2",
    ),
    BadgeThemePreviewLane(
        kind = BadgeThemePreviewLaneKind.Ble,
        token = "03 BLE",
        title = "BLE TRACKER",
        detail = "FOLLOWING  •  -67 dB",
        state = "NEAR",
        accentKey = "tracker",
        testTag = "badge_theme_preview_ble",
    ),
    BadgeThemePreviewLane(
        kind = BadgeThemePreviewLaneKind.Wifi,
        token = "04 WIFI",
        title = "EVIL TWIN",
        detail = "OPEN CLONE  •  -48 dB",
        state = "ALERT",
        accentKey = "wifi_attack",
        testTag = "badge_theme_preview_wifi",
    ),
)

@Composable
fun BadgeThemePreview(
    theme: BadgeTheme,
    modifier: Modifier = Modifier,
) {
    val normalized = theme.normalizedV1()
    val chrome = previewChrome(normalized.palette)
    val background = previewBackground(normalized.palette, normalized.background)
    val panel = previewPanel(normalized.palette)
    val scanlineModifier = if (normalized.background == "scanline") {
        Modifier.drawBehind {
            var y = 0f
            while (y < size.height) {
                drawLine(
                    color = Color.White.copy(alpha = 0.035f),
                    start = androidx.compose.ui.geometry.Offset(0f, y),
                    end = androidx.compose.ui.geometry.Offset(size.width, y),
                    strokeWidth = 1f,
                )
                y += 7f
            }
        }
    } else {
        Modifier
    }

    Box(
        modifier = modifier.fillMaxWidth(),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = Modifier
                .fillMaxWidth(0.74f)
                .widthIn(max = 260.dp)
                .aspectRatio(0.8f)
                .testTag("badge_theme_preview")
                .semantics {
                    contentDescription =
                        "Badge preview with two global lanes, one BLE lane, one Wi-Fi lane, and scanner health"
                },
            shape = RoundedCornerShape(14.dp),
            color = background,
            border = BorderStroke(1.dp, chrome.copy(alpha = 0.72f)),
            tonalElevation = 0.dp,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .then(scanlineModifier)
                    .padding(8.dp),
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(24.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "FRIEND / FOE",
                        color = chrome,
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Black,
                        letterSpacing = 0.9.sp,
                    )
                    Spacer(modifier = Modifier.weight(1f))
                    Text(
                        text = "${normalized.palette.uppercase()}  ${normalized.brightness}%",
                        color = chrome.copy(alpha = 0.78f),
                        fontSize = 7.sp,
                        fontWeight = FontWeight.Bold,
                    )
                }

                BadgeThemePreviewLanes.forEach { lane ->
                    val fallback = BadgeThemeAccentClasses
                        .first { it.key == lane.accentKey }
                        .defaultRgb565
                    BadgeThemePreviewLaneRow(
                        lane = lane,
                        accent = rgb565Color(normalized.accents[lane.accentKey] ?: fallback),
                        panel = panel,
                        chrome = chrome,
                        modifier = Modifier.weight(1f),
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                }

                val healthFallback = BadgeThemeAccentClasses
                    .first { it.key == "clear" }
                    .defaultRgb565
                val health = rgb565Color(normalized.accents["clear"] ?: healthFallback)
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(24.dp)
                        .background(health.copy(alpha = 0.13f), RoundedCornerShape(5.dp))
                        .testTag("badge_theme_preview_health")
                        .semantics {
                            contentDescription = "Scanner health: BLE okay, Wi-Fi okay, four lanes"
                        }
                        .padding(horizontal = 7.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Box(
                        modifier = Modifier
                            .width(4.dp)
                            .height(4.dp)
                            .background(health, RoundedCornerShape(50)),
                    )
                    Text(
                        text = "BLE OK  •  WIFI OK",
                        color = health,
                        fontSize = 7.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(modifier = Modifier.weight(1f))
                    Text(
                        text = "4/4",
                        color = chrome,
                        fontSize = 7.sp,
                        fontWeight = FontWeight.Bold,
                    )
                }
            }
        }
    }
}

@Composable
private fun BadgeThemePreviewLaneRow(
    lane: BadgeThemePreviewLane,
    accent: Color,
    panel: Color,
    chrome: Color,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(panel, RoundedCornerShape(6.dp))
            .testTag(lane.testTag)
            .semantics {
                contentDescription = "${lane.token}: ${lane.title}, ${lane.detail}, ${lane.state}"
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .width(4.dp)
                .fillMaxHeight()
                .background(accent, RoundedCornerShape(topStart = 6.dp, bottomStart = 6.dp)),
        )
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(horizontal = 7.dp, vertical = 4.dp),
        ) {
            Text(
                text = lane.token,
                color = accent,
                fontSize = 6.sp,
                fontWeight = FontWeight.Black,
                letterSpacing = 0.65.sp,
                maxLines = 1,
            )
            Text(
                text = lane.title,
                color = Color.White.copy(alpha = 0.94f),
                fontSize = 9.sp,
                fontWeight = FontWeight.Black,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = lane.detail,
                color = chrome.copy(alpha = 0.72f),
                fontSize = 6.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Surface(
            modifier = Modifier.padding(end = 6.dp),
            shape = RoundedCornerShape(4.dp),
            color = accent.copy(alpha = 0.16f),
            border = BorderStroke(0.5.dp, accent.copy(alpha = 0.7f)),
        ) {
            Text(
                text = lane.state,
                color = accent,
                fontSize = 6.sp,
                fontWeight = FontWeight.Black,
                modifier = Modifier.padding(horizontal = 5.dp, vertical = 3.dp),
            )
        }
    }
}

internal fun rgb565Color(rgb565: Int): Color {
    val rgb = BadgeThemeColorCodec.rgb565ToRgb888(rgb565.coerceIn(0, 0xFFFF))
    return Color(rgb.red, rgb.green, rgb.blue)
}

private fun previewChrome(palette: String): Color = when (palette) {
    "night" -> Color(0xFF9CAAC1)
    "neon" -> Color(0xFFC681FF)
    "mono" -> Color(0xFFD8F6F7)
    else -> Color(0xFF6DFF9A)
}

private fun previewBackground(palette: String, background: String): Color {
    val base = when (palette) {
        "night" -> Color(0xFF070A10)
        "neon" -> Color(0xFF100518)
        "mono" -> Color(0xFF060B0C)
        else -> Color(0xFF04110B)
    }
    return if (background == "dim") base.copy(alpha = 0.86f) else base
}

private fun previewPanel(palette: String): Color = when (palette) {
    "night" -> Color(0xFF121722)
    "neon" -> Color(0xFF1C0B29)
    "mono" -> Color(0xFF101819)
    else -> Color(0xFF0A1C14)
}
