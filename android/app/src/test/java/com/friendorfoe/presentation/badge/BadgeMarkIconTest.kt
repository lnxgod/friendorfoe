package com.friendorfoe.presentation.badge

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeMarkIconTest {

    @Test
    fun `badge mark is three separated triangles in a 24dp vector`() {
        assertEquals(
            listOf(
                BadgeTriangle(12f, 2f, 7f, 10f, 17f),
                BadgeTriangle(6.5f, 11f, 1.5f, 19f, 11.5f),
                BadgeTriangle(17.5f, 11f, 12.5f, 19f, 22.5f),
            ),
            BadgeMarkTriangles,
        )
        assertTrue(BadgeMarkTriangles[0].baseY < BadgeMarkTriangles[1].topY)
        assertEquals(24.dp, BadgeMarkIcon.defaultWidth)
        assertEquals(24.dp, BadgeMarkIcon.defaultHeight)
        assertEquals(24f, BadgeMarkIcon.viewportWidth)
        assertEquals(24f, BadgeMarkIcon.viewportHeight)
    }

    @Test
    fun `selected badge mark color is gold`() {
        assertEquals(Color(0xFFFFC107), BadgeMarkGold)
    }
}
