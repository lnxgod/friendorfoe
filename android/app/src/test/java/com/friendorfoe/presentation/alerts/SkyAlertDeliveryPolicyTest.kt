package com.friendorfoe.presentation.alerts

import org.junit.Assert.*
import org.junit.Test

class SkyAlertDeliveryPolicyTest {
    @Test fun undeliveredAttemptDoesNotConsumeCooldown() {
        val policy = SkyAlertPolicy(cooldownMs = 600_000)
        val candidate = SkyAlertCandidate("key", "title", "body", objectId = "abc123")
        assertTrue(policy.isEligible(candidate, 1000))
        // Permission denial or a failed delivery leaves the candidate eligible.
        assertTrue(policy.isEligible(candidate, 2000))
        policy.markDelivered(candidate, 2000)
        assertFalse(policy.isEligible(candidate, 3000))
        assertTrue(policy.isEligible(candidate, 602_000))
    }
    @Test fun snoozeExpiryAndObjectMuteAreIndependent() {
        val state = SkyAlertControlState(30_000, setOf("muted"))
        assertFalse(state.allows("other", 29_999))
        assertTrue(state.allows("other", 30_000))
        assertFalse(state.allows("muted", 50_000))
    }
    @Test fun notificationRoutingRetainsExactIdentityAndRejectsInvalidPayloads() {
        assertEquals("RID:device/123", SkyAlertRoute.parse(SkyAlertRoute.OPEN, "RID:device/123"))
        assertNull(SkyAlertRoute.parse(null, "abc123"))
        assertNull(SkyAlertRoute.parse(SkyAlertRoute.OPEN, "\nabc"))
        assertNull(SkyAlertRoute.parse(SkyAlertRoute.OPEN, "x".repeat(257)))
    }
}
