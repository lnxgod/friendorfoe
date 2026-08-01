package com.friendorfoe.presentation.privacy

import android.app.NotificationManager
import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class PrivacyAlertNotifierTest {
    @Test
    fun globallyDisabledNotificationsReturnFalseBeforeAllocatingOrPosting() {
        val harness = notifier(PrivacyNotificationDeliveryState.GLOBAL_DISABLED)

        assertFalse(harness.notifier.publish(finding()))
        assertEquals(0, harness.idCalls)
        assertEquals(0, harness.postCalls)
        assertEquals(
            PrivacyNotificationDeliveryState.GLOBAL_DISABLED,
            privacyNotificationDeliveryState(
                hasRuntimePermission = true,
                globallyEnabled = false,
                channelImportance = NotificationManager.IMPORTANCE_HIGH,
            ),
        )
    }

    @Test
    fun blockedPrivacyChannelReturnsFalseBeforeAllocatingOrPosting() {
        val harness = notifier(PrivacyNotificationDeliveryState.CHANNEL_BLOCKED)

        assertFalse(harness.notifier.publish(finding()))
        assertEquals(0, harness.idCalls)
        assertEquals(0, harness.postCalls)
        assertEquals(
            PrivacyNotificationDeliveryState.CHANNEL_BLOCKED,
            privacyNotificationDeliveryState(
                hasRuntimePermission = true,
                globallyEnabled = true,
                channelImportance = NotificationManager.IMPORTANCE_NONE,
            ),
        )
    }

    private fun notifier(state: PrivacyNotificationDeliveryState): NotifierHarness {
        var idCalls = 0
        var postCalls = 0
        val notifier = PrivacyAlertNotifier(
            notificationsEnabled = { true },
            deliveryState = { state },
            ids = PrivacyNotificationIdStore {
                idCalls += 1
                7
            },
            post = { _, _ ->
                postCalls += 1
                true
            },
        )
        return NotifierHarness(
            notifier = notifier,
            idCallsValue = { idCalls },
            postCallsValue = { postCalls },
        )
    }

    private fun finding() = PrivacyFinding(
        displayId = "critical",
        observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "observation:42"),
        source = PrivacySourceKind.BACKEND,
        stableSourceId = "stable:42",
        routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:42"),
        title = "Critical finding",
        evidence = "Current evidence",
        limitation = null,
        category = PrivacyCategory.HIDDEN_CAMERA,
        severity = FindingSeverity.CRITICAL,
        ownership = Ownership.UNKNOWN,
        signalDbm = null,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = false,
    )

    private class NotifierHarness(
        val notifier: PrivacyAlertNotifier,
        private val idCallsValue: () -> Int,
        private val postCallsValue: () -> Int,
    ) {
        val idCalls: Int get() = idCallsValue()
        val postCalls: Int get() = postCallsValue()
    }
}
