package com.friendorfoe.presentation.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class PrivacyNotificationRouteTest {
    @Test
    fun notificationIdentityIncludesExactSourceAndRecord() {
        val ids = RecordingIdStore()
        val key = PrivacyFindingKey(PrivacySourceKind.BADGE_USB, "entity:42")

        val target = PrivacyNotificationRoute.from(key, ids)
        val other = PrivacyNotificationRoute.from(
            PrivacyFindingKey(PrivacySourceKind.BADGE_USB, "entity:43"),
            ids,
        )

        assertEquals("privacy/finding/badge_usb/entity%3A42", target.route)
        assertEquals(
            "friendorfoe://privacy/finding/badge_usb/entity%3A42",
            target.dataUri,
        )
        assertNotEquals(target.pendingIntentId, other.pendingIntentId)
        assertNotEquals(target.dataUri, other.dataUri)
    }

    private class RecordingIdStore : PrivacyNotificationIdStore {
        private val ids = linkedMapOf<PrivacyFindingKey, Int>()

        override fun idFor(key: PrivacyFindingKey): Int =
            ids.getOrPut(key) { ids.size + 1 }
    }
}
