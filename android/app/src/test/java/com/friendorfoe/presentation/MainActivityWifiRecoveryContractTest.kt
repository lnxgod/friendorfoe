package com.friendorfoe.presentation

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MainActivityWifiRecoveryContractTest {

    @Test
    fun permission_result_and_activity_resume_wake_wifi_scanning() {
        val source = mainActivitySource()

        assertTrue(source.contains("lateinit var wifiScanCoordinator: WifiScanCoordinator"))
        assertTrue(source.contains("override fun onResume()"))
        assertTrue(source.contains("wifiScanCoordinator.notifyPlatformStateChanged()"))
        assertTrue(source.contains("onPlatformStateChanged: () -> Unit"))
        assertTrue(source.contains(") { onPlatformStateChanged() }"))
        assertFalse(source.contains("/* grant results handled by individual screens */"))
    }

    private fun mainActivitySource(): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/presentation/MainActivity.kt"),
            File("app/src/main/java/com/friendorfoe/presentation/MainActivity.kt"),
            File("android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt"),
        )
        return candidates.first { it.isFile }.readText()
    }
}
