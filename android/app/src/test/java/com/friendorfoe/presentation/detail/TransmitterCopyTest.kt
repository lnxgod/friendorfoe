package com.friendorfoe.presentation.detail

import org.junit.Assert.assertEquals
import org.junit.Test

class TransmitterCopyTest {

    @Test
    fun bssid_copy_describes_observed_radio_without_overclaiming() {
        assertEquals("Observed BSSID / MAC", OBSERVED_BSSID_LABEL)
        assertEquals(
            "May rotate or belong to the aircraft/controller radio.",
            OBSERVED_BSSID_EXPLANATION,
        )
    }
}
