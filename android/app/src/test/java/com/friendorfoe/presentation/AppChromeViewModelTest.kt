package com.friendorfoe.presentation

import com.friendorfoe.data.remote.EventStatsDto
import com.friendorfoe.data.remote.FakeSensorMapApiService
import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AppChromeViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun refreshNow_sums_probe_intel_unacknowledged_counts() = runTest {
        val api = FakeSensorMapApiService().apply {
            eventStats = EventStatsDto(
                unackByType = mapOf(
                    "new_probe_identity" to 2,
                    "new_probe_mac" to 1,
                    "new_probed_ssid" to 3,
                    "probe_activity_spike" to 1,
                    "new_rid_drone" to 9,
                )
            )
        }
        val viewModel = AppChromeViewModel(api)

        viewModel.refreshNow()
        advanceUntilIdle()

        assertEquals(7, viewModel.calibrateBadgeCount.value)
    }
}
