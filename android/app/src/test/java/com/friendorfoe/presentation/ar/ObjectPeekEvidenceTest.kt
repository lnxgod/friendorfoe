package com.friendorfoe.presentation.ar

import com.friendorfoe.domain.model.DetectionSource
import org.junit.Assert.assertEquals
import org.junit.Test

class ObjectPeekEvidenceTest {
    @Test
    fun remoteIdWifiTransportsRemainRemoteIdEvidence() {
        assertEquals(
            "Remote ID radio match (Wi-Fi NaN)",
            objectPeekEvidence(DetectionSource.WIFI_NAN),
        )
        assertEquals(
            "Remote ID radio match (Wi-Fi Beacon)",
            objectPeekEvidence(DetectionSource.WIFI_BEACON),
        )
        assertEquals("Wi-Fi observation", objectPeekEvidence(DetectionSource.WIFI))
    }
}
