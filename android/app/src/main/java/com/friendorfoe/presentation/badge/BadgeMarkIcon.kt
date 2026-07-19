package com.friendorfoe.presentation.badge

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

internal data class BadgeTriangle(
    val topX: Float,
    val topY: Float,
    val leftX: Float,
    val baseY: Float,
    val rightX: Float,
)

internal val BadgeMarkTriangles = listOf(
    BadgeTriangle(12f, 2f, 7f, 10f, 17f),
    BadgeTriangle(6.5f, 11f, 1.5f, 19f, 11.5f),
    BadgeTriangle(17.5f, 11f, 12.5f, 19f, 22.5f),
)

val BadgeMarkGold = Color(0xFFFFC107)

val BadgeMarkIcon: ImageVector by lazy {
    ImageVector.Builder(
        name = "BadgeMark",
        defaultWidth = 24.dp,
        defaultHeight = 24.dp,
        viewportWidth = 24f,
        viewportHeight = 24f,
    ).apply {
        BadgeMarkTriangles.forEach { triangle ->
            path(fill = SolidColor(Color.Black)) {
                moveTo(triangle.topX, triangle.topY)
                lineTo(triangle.leftX, triangle.baseY)
                lineTo(triangle.rightX, triangle.baseY)
                close()
            }
        }
    }.build()
}
