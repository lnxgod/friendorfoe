package com.friendorfoe.data.badge

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbFrameTrustTest {

    @Test
    fun `unverified non-handshake frames cannot mutate badge state`() {
        assertFalse(
            badgeUsbFrameMayMutateState(
                hasVerifiedOwner = false,
                acceptedIdentityHandshake = false,
            )
        )
    }

    @Test
    fun `accepted identity status and verified owner frames may mutate badge state`() {
        assertTrue(
            badgeUsbFrameMayMutateState(
                hasVerifiedOwner = false,
                acceptedIdentityHandshake = true,
            )
        )
        assertTrue(
            badgeUsbFrameMayMutateState(
                hasVerifiedOwner = true,
                acceptedIdentityHandshake = false,
            )
        )
    }
}
