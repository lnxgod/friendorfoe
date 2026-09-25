package com.friendorfoe.presentation.theme

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat

private val AppTypography = Typography(
    displaySmall = TextStyle(fontWeight = FontWeight.Bold, fontSize = 36.sp, lineHeight = 44.sp),
    headlineMedium = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 28.sp, lineHeight = 36.sp),
    headlineSmall = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 24.sp, lineHeight = 32.sp),
    titleLarge = TextStyle(fontWeight = FontWeight.Medium, fontSize = 22.sp, lineHeight = 28.sp),
    titleMedium = TextStyle(fontWeight = FontWeight.Medium, fontSize = 16.sp, lineHeight = 24.sp, letterSpacing = 0.15.sp),
    titleSmall = TextStyle(fontWeight = FontWeight.Medium, fontSize = 14.sp, lineHeight = 20.sp, letterSpacing = 0.1.sp),
    bodyLarge = TextStyle(fontSize = 16.sp, lineHeight = 24.sp, letterSpacing = 0.sp),
    bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 20.sp, letterSpacing = 0.sp),
    bodySmall = TextStyle(fontSize = 12.sp, lineHeight = 16.sp, letterSpacing = 0.sp),
    labelLarge = TextStyle(fontWeight = FontWeight.Medium, fontSize = 14.sp, lineHeight = 20.sp, letterSpacing = 0.1.sp),
    labelMedium = TextStyle(fontWeight = FontWeight.Medium, fontSize = 12.sp, lineHeight = 16.sp, letterSpacing = 0.sp),
    labelSmall = TextStyle(fontWeight = FontWeight.Medium, fontSize = 11.sp, lineHeight = 16.sp, letterSpacing = 0.sp),
)

private val AppShapes = Shapes(
    small = RoundedCornerShape(8.dp),
    medium = RoundedCornerShape(8.dp),
    large = RoundedCornerShape(12.dp),
    extraLarge = RoundedCornerShape(16.dp),
)

// Neutral surfaces keep observations legible; color is reserved for actions and meaning.
private val DarkColorScheme = darkColorScheme(
    primary = Color(0xFF83DFC4), onPrimary = Color(0xFF00382C),
    primaryContainer = Color(0xFF1B4036), onPrimaryContainer = Color(0xFFB4F3DF),
    secondary = Color(0xFFB5CCC3), onSecondary = Color(0xFF20362F),
    secondaryContainer = Color(0xFF263F36), onSecondaryContainer = Color(0xFFD2E9DF),
    tertiary = Color(0xFFEBC17C), onTertiary = Color(0xFF412D08),
    background = Color(0xFF0E1518), onBackground = Color(0xFFEBF1EE),
    surface = Color(0xFF0E1518), onSurface = Color(0xFFEBF1EE),
    surfaceDim = Color(0xFF0E1518), surfaceBright = Color(0xFF303B3C),
    surfaceContainerLowest = Color(0xFF0A1012), surfaceContainerLow = Color(0xFF141D20),
    surfaceContainer = Color(0xFF192326), surfaceContainerHigh = Color(0xFF222E30),
    surfaceContainerHighest = Color(0xFF2B3739),
    surfaceVariant = Color(0xFF253235), onSurfaceVariant = Color(0xFFA9BAB7),
    outline = Color(0xFF829592), outlineVariant = Color(0xFF31413F),
    error = Color(0xFFFFB4AB), onError = Color(0xFF690005),
    errorContainer = Color(0xFF6E2725), onErrorContainer = Color(0xFFFFDAD6),
)

private val LightColorScheme = lightColorScheme(
    primary = Color(0xFF006C53), onPrimary = Color.White,
    primaryContainer = Color(0xFFD2F2E5), onPrimaryContainer = Color(0xFF00382A),
    secondary = Color(0xFF476457), onSecondary = Color.White,
    secondaryContainer = Color(0xFFDCECE3), onSecondaryContainer = Color(0xFF18352A),
    tertiary = Color(0xFF805600), onTertiary = Color.White,
    background = Color(0xFFF8FAF7), onBackground = Color(0xFF18221F),
    surface = Color(0xFFF8FAF7), onSurface = Color(0xFF18221F),
    surfaceDim = Color(0xFFD9E2DC), surfaceBright = Color(0xFFF8FAF7),
    surfaceContainerLowest = Color.White, surfaceContainerLow = Color(0xFFF1F5F0),
    surfaceContainer = Color(0xFFEBF0EA), surfaceContainerHigh = Color(0xFFE5EBE5),
    surfaceContainerHighest = Color(0xFFDFE6DF),
    surfaceVariant = Color(0xFFE3EBE5), onSurfaceVariant = Color(0xFF4E625B),
    outline = Color(0xFF6E8178), outlineVariant = Color(0xFFD1DBD3),
    error = Color(0xFFAD2927), onError = Color.White,
    errorContainer = Color(0xFFFFDAD6), onErrorContainer = Color(0xFF410002),
)

@Composable
fun FriendOrFoeTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    val colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = Color.Transparent.toArgb()
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = !darkTheme
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = AppTypography,
        shapes = AppShapes,
        content = content
    )
}
