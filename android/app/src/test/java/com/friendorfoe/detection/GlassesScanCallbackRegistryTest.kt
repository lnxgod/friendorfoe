package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class GlassesScanCallbackRegistryTest {

    @Test
    fun failedOverlapStopStaysTrackedAndCloseRetriesEveryRegisteredCallback() {
        val stops = mutableListOf<String>()
        var oldFailuresRemaining = 1
        val registry = GlassesScanCallbackRegistry<String> { callback ->
            stops += callback
            if (callback == "old" && oldFailuresRemaining-- > 0) {
                error("temporary stop failure")
            }
        }
        assertEquals(GlassesScanEvent.Ready, registry.register("old"))
        assertEquals(GlassesScanEvent.Ready, registry.register("new"))

        assertFalse(registry.stop("old"))
        assertEquals(2, registry.registeredCount)

        registry.closeAndStopAll()

        assertEquals(listOf("old", "old", "new"), stops)
        assertEquals(0, registry.registeredCount)
    }

    @Test
    fun callbackRegisteredAfterCloseIsStoppedImmediatelyAndNeverTracked() {
        val stops = mutableListOf<String>()
        val registry = GlassesScanCallbackRegistry<TestRegistration> {
            stops += "${it.scanner}:${it.callback}"
        }
        registry.closeAndStopAll()

        assertEquals(null, registry.register(TestRegistration("scanner-b", "late")))

        assertEquals(listOf("scanner-b:late"), stops)
        assertEquals(0, registry.registeredCount)
    }

    @Test
    fun failedLateStopRemainsTrackedSoCloseCanRetryIt() {
        val stops = mutableListOf<String>()
        var failuresRemaining = 1
        val registry = GlassesScanCallbackRegistry<String> { callback ->
            stops += callback
            if (failuresRemaining-- > 0) error("temporary late stop failure")
        }
        registry.closeAndStopAll()

        assertEquals(null, registry.register("late"))
        assertEquals(1, registry.registeredCount)

        registry.closeAndStopAll()
        assertEquals(listOf("late", "late"), stops)
        assertEquals(0, registry.registeredCount)
    }

    private data class TestRegistration(val scanner: String, val callback: String)
}
