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

    @Test
    fun `coded extended adapter scans every supported remote id phy`() {
        assertTrue(
            useAllSupportedRemoteIdPhy(
                extendedAdvertisingSupported = true,
                codedPhySupported = true,
            )
        )
    }

    @Test
    fun `legacy or uncoded adapter keeps the default one megabit phy`() {
        assertFalse(
            useAllSupportedRemoteIdPhy(
                extendedAdvertisingSupported = false,
                codedPhySupported = true,
            )
        )
        assertFalse(
            useAllSupportedRemoteIdPhy(
                extendedAdvertisingSupported = true,
                codedPhySupported = false,
            )
        )
    }
}
