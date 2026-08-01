package com.friendorfoe.presentation.privacy

import com.friendorfoe.presentation.navigation.Screen
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyLaunchRouteTest {
    @Test
    fun notificationDataAndExtraMustResolveToTheSameExactFinding() {
        val key = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:42/alpha")
        val route = Screen.PrivacyFinding.createRoute(key)
        val data = "friendorfoe://privacy/finding/backend/entity%3A42%2Falpha"

        assertEquals(route, PrivacyLaunchRoute.parse(data, route))
        assertNull(
            PrivacyLaunchRoute.parse(
                data,
                Screen.PrivacyFinding.createRoute(
                    PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:43"),
                ),
            ),
        )
    }

    @Test
    fun malformedOrUnknownSourcesNeverEnterThePendingQueue() {
        val queue = PendingPrivacyRouteQueue()

        assertFalse(queue.offer("friendorfoe://privacy/finding/nope/entity%3A42", null))
        assertFalse(queue.offer(null, "privacy/finding/nope/entity%3A42"))
        assertNull(queue.pending.value)
    }

    @Test
    fun pendingRouteKeepsTheLatestValidExactTargetAndConsumesItOnce() {
        val queue = PendingPrivacyRouteQueue()
        val first = "privacy/finding/phone_ble/mac%3AAA"
        val latest = "privacy/finding/wifi_analysis/bssid%3ABB"

        assertTrue(queue.offer(null, first))
        assertTrue(queue.offer(null, latest))
        assertEquals(latest, queue.pending.value)
        assertFalse(queue.consume(first))
        assertTrue(queue.consume(latest))
        assertNull(queue.pending.value)
        assertFalse(queue.consume(latest))
    }

    @Test
    fun acceptedIntentPayloadIsClearedSoRecreationCannotRequeueIt() {
        val queue = PendingPrivacyRouteQueue()
        val payload = PrivacyLaunchIntentPayload(
            dataUri = "friendorfoe://privacy/finding/backend/entity%3A42",
            routeExtra = "privacy/finding/backend/entity%3A42",
        )

        val cleared = acceptPrivacyLaunchIntent(queue, payload)
        val savedPendingRoute = queue.savedRoute()
        val recreated = PendingPrivacyRouteQueue(savedPendingRoute)
        val route = requireNotNull(recreated.pending.value)

        assertNull(cleared.dataUri)
        assertNull(cleared.routeExtra)
        assertEquals("privacy/finding/backend/entity%3A42", route)
        assertFalse(recreated.offer(cleared.dataUri, cleared.routeExtra))
        assertTrue(recreated.consume(route))
        assertNull(recreated.savedRoute())
    }
}
