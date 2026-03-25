package com.friendorfoe.presentation.util

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import com.friendorfoe.domain.model.ObjectCategory

/**
 * Map an [ObjectCategory] to its overlay color (light theme default).
 * For non-Composable contexts (map markers, Canvas drawing).
 */
fun categoryColor(category: ObjectCategory): Color {
    return categoryColorLight(category)
}

private fun categoryColorLight(category: ObjectCategory): Color {
    return when (category) {
        ObjectCategory.COMMERCIAL -> Color(0xFF4CAF50)       // Green
        ObjectCategory.GENERAL_AVIATION -> Color(0xFFFFA726) // Orange
        ObjectCategory.MILITARY -> Color(0xFFF44336)         // Red
        ObjectCategory.HELICOPTER -> Color(0xFF26A69A)       // Teal
        ObjectCategory.GOVERNMENT -> Color(0xFFE65100)       // Deep Orange
        ObjectCategory.EMERGENCY -> Color(0xFFE91E63)        // Magenta/Pink
        ObjectCategory.CARGO -> Color(0xFF8D6E63)            // Brown
        ObjectCategory.DRONE -> Color(0xFF2196F3)            // Blue
        ObjectCategory.GROUND_VEHICLE -> Color(0xFF616161)   // Dark Gray
        ObjectCategory.UNKNOWN -> Color(0xFF9E9E9E)          // Gray
    }
}

private fun categoryColorDark(category: ObjectCategory): Color {
    return when (category) {
        ObjectCategory.COMMERCIAL -> Color(0xFF66BB6A)       // Lighter green
        ObjectCategory.GENERAL_AVIATION -> Color(0xFFFFCC80) // Lighter orange
        ObjectCategory.MILITARY -> Color(0xFFEF5350)         // Lighter red
        ObjectCategory.HELICOPTER -> Color(0xFF4DB6AC)       // Lighter teal
        ObjectCategory.GOVERNMENT -> Color(0xFFFF8A65)       // Lighter deep orange
        ObjectCategory.EMERGENCY -> Color(0xFFF06292)        // Lighter pink
        ObjectCategory.CARGO -> Color(0xFFA1887F)            // Lighter brown
        ObjectCategory.DRONE -> Color(0xFF64B5F6)            // Lighter blue
        ObjectCategory.GROUND_VEHICLE -> Color(0xFF9E9E9E)   // Lighter gray
        ObjectCategory.UNKNOWN -> Color(0xFFBDBDBD)          // Lighter gray
    }
}

/**
 * Theme-aware category color. Use this in Composable contexts.
 */
@Composable
fun categoryColorThemed(category: ObjectCategory): Color {
    return if (isSystemInDarkTheme()) categoryColorDark(category) else categoryColorLight(category)
}

/**
 * Map an [ObjectCategory] to an Android Color int (ARGB) for use with
 * osmdroid markers and other Android graphics APIs.
 */
fun categoryColorArgb(category: ObjectCategory): Int {
    return when (category) {
        ObjectCategory.COMMERCIAL -> 0xFF4CAF50.toInt()
        ObjectCategory.GENERAL_AVIATION -> 0xFFFFA726.toInt()
        ObjectCategory.MILITARY -> 0xFFF44336.toInt()
        ObjectCategory.HELICOPTER -> 0xFF26A69A.toInt()
        ObjectCategory.GOVERNMENT -> 0xFFE65100.toInt()
        ObjectCategory.EMERGENCY -> 0xFFE91E63.toInt()
        ObjectCategory.CARGO -> 0xFF8D6E63.toInt()
        ObjectCategory.DRONE -> 0xFF2196F3.toInt()
        ObjectCategory.GROUND_VEHICLE -> 0xFF616161.toInt()
        ObjectCategory.UNKNOWN -> 0xFF9E9E9E.toInt()
    }
}

/**
 * Overload accepting a category string (e.g. from Room entity).
 */
fun categoryColor(category: String): Color {
    val parsed = try { ObjectCategory.valueOf(category.uppercase()) }
                 catch (_: IllegalArgumentException) { ObjectCategory.UNKNOWN }
    return categoryColor(parsed)
}

/**
 * Returns a short badge label and color for special categories, or null
 * for categories that don't need a badge (COMMERCIAL, GA, DRONE, UNKNOWN).
 */
fun categoryBadge(category: ObjectCategory): Pair<String, Color>? {
    return when (category) {
        ObjectCategory.MILITARY -> "MIL" to Color(0xFFF44336)
        ObjectCategory.GOVERNMENT -> "GOV" to Color(0xFFE65100)
        ObjectCategory.HELICOPTER -> "HELI" to Color(0xFF26A69A)
        ObjectCategory.EMERGENCY -> "EMG" to Color(0xFFE91E63)
        ObjectCategory.CARGO -> "CGO" to Color(0xFF8D6E63)
        ObjectCategory.GROUND_VEHICLE -> "GND" to Color(0xFF616161)
        else -> null
    }
}

fun isMilitary(category: ObjectCategory): Boolean =
    category == ObjectCategory.MILITARY || category == ObjectCategory.GOVERNMENT

fun isMilitary(category: String): Boolean =
    category.equals("military", ignoreCase = true) || category.equals("government", ignoreCase = true)
