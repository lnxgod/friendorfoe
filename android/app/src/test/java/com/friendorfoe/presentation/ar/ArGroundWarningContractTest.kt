package com.friendorfoe.presentation.ar

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ArGroundWarningContractTest {

    @Test
    fun removes_camera_direction_warning_without_removing_pitch_tracking() {
        val source = arViewSource()

        assertFalse(source.contains("showGroundBanner"))
        assertFalse(source.contains("Camera pointing below horizon"))
        assertTrue(source.contains("currentPitch = orientation.pitchDegrees"))
        assertTrue(source.contains("pitchDegrees = orientation.pitchDegrees"))
    }

    private fun arViewSource(): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt"),
            File("app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt"),
            File("android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt"),
        )
        return candidates.first { it.isFile }.readText()
    }
}
