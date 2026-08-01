package com.friendorfoe.data

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AppVersionTest {
    @Test
    fun unequalLabelsDoNotAutomaticallyMeanUpdate() {
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65-privacy-beacons"),
                remote = AppVersion(null, "0.64.65"),
            ),
        )
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(107, "9.0.0"),
            ),
        )
        assertTrue(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(109, "0.1.0"),
            ),
        )
        assertTrue(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(null, "0.65.0"),
            ),
        )
    }

    @Test
    fun malformedRemoteVersionIsNotAnUpdate() {
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(null, "latest"),
            ),
        )
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(null, "0.65.0garbage"),
            ),
        )
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65"),
                remote = AppVersion(null, "0.1000000.0"),
            ),
        )
        assertTrue(
            isUpdateAvailable(
                installed = AppVersion(108, "0.64.65-dev+4"),
                remote = AppVersion(null, "v0.65.0-release+1"),
            ),
        )
    }

    @Test
    fun missingNumericSegmentsCompareAsZero() {
        assertFalse(
            isUpdateAvailable(
                installed = AppVersion(null, "1.2.0"),
                remote = AppVersion(null, "v1.2"),
            ),
        )
        assertTrue(
            isUpdateAvailable(
                installed = AppVersion(null, "1.2.9"),
                remote = AppVersion(null, "1.3"),
            ),
        )
    }
}
