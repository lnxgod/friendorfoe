package com.friendorfoe.data.repository

import com.google.android.play.core.install.model.UpdateAvailability
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AppUpdateRepositoryTest {
    @Test
    fun playDecidesWhetherAnUpdateIsAvailableToThisAccount() {
        assertEquals(PlayUpdateStatus.CURRENT, playUpdateStatus(UpdateAvailability.UPDATE_NOT_AVAILABLE).getOrThrow())
        assertEquals(PlayUpdateStatus.AVAILABLE, playUpdateStatus(UpdateAvailability.UPDATE_AVAILABLE).getOrThrow())
        assertEquals(PlayUpdateStatus.AVAILABLE,
            playUpdateStatus(UpdateAvailability.DEVELOPER_TRIGGERED_UPDATE_IN_PROGRESS).getOrThrow())
        assertTrue(playUpdateStatus(UpdateAvailability.UNKNOWN).isFailure)
    }
}
