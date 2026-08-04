package com.friendorfoe.detection

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RemoteIdScannerPolicyTest {

    @Test
    fun `extended advertising disables legacy-only scanning`() {
        assertFalse(useLegacyRemoteIdScan(extendedAdvertisingSupported = true))
    }

    @Test
    fun `adapter without extended advertising remains legacy-only`() {
        assertTrue(useLegacyRemoteIdScan(extendedAdvertisingSupported = false))
    }
}
