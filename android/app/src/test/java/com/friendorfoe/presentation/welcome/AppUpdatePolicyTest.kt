package com.friendorfoe.presentation.welcome

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AppUpdatePolicyTest {
    @Test
    fun `recognizes a newer patch release after version suffixes`() {
        assertTrue(
            AppUpdatePolicy.isRemoteNewer(
                "0.64.68-live-follow",
                "v0.64.70-android-defcon34-badge-ui"
            )
        )
    }

    @Test
    fun `recognizes the next patch release`() {
        assertTrue(AppUpdatePolicy.isRemoteNewer("0.64.69-local", "v0.64.70-remote"))
    }

    @Test
    fun `ignores suffix differences for equal semantic versions`() {
        assertFalse(AppUpdatePolicy.isRemoteNewer("0.64.70-local", "v0.64.70-remote"))
    }

    @Test
    fun `does not offer an older remote release`() {
        assertFalse(AppUpdatePolicy.isRemoteNewer("0.64.70", "v0.64.68"))
    }

    @Test
    fun `compares multi-digit patch components numerically`() {
        assertTrue(AppUpdatePolicy.isRemoteNewer("0.64.99", "v0.64.100"))
    }

    @Test
    fun `rejects malformed remote versions`() {
        assertFalse(AppUpdatePolicy.isRemoteNewer("0.64.69", "v0.64.release"))
    }
}
