package com.friendorfoe.presentation.watch

import org.junit.Assert.*
import org.junit.Test

class WatchLocationTest {
    @Test fun absentFutureAndExpiredLocationsCannotDriveBackgroundAlerts() {
        val now = 200_000_000_000L
        assertFalse(freshWatchLocation(null, now))
        assertFalse(freshWatchLocation(now + 1, now))
        assertFalse(freshWatchLocation(now - 120_000_000_001L, now))
        assertTrue(freshWatchLocation(now - 120_000_000_000L, now))
        assertTrue(freshWatchLocation(now, now))
    }
}
