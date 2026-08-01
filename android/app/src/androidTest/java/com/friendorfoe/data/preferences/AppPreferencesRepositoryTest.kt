package com.friendorfoe.data.preferences

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AppPreferencesRepositoryTest {

    private val context
        get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun onboardingRouteAndIgnoredKeysSurviveRepositoryRecreation() = runBlocking {
        val first = AppPreferencesRepository(context)
        first.resetForInstrumentation()
        try {
            val ignored = FindingPreferenceKey.create("phone_ble", "AA:BB")!!
            first.setOnboardingComplete()
            first.setLastTopLevelRoute("privacy")
            first.ignoreFinding(ignored)

            val recreated = AppPreferencesRepository(context)
            assertEquals(AppLaunchState.Ready("privacy"), recreated.launchState.first())
            assertEquals(setOf(ignored.encoded), recreated.ignoredFindingKeys.first())

            recreated.restoreFinding(ignored)
            assertTrue(recreated.ignoredFindingKeys.first().isEmpty())
        } finally {
            first.resetForInstrumentation()
        }
    }
}
