package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyDetectionOrigin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class BleInvestigationRoutingTest {
    private val now = 20_000L

    @Test
    fun `ANDROID AUTO with phone capability selects phone`() {
        val decision = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.ANDROID),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertEquals(BleInvestigationRoute.PHONE, decision.route)
        assertNull(decision.error)
    }

    @Test
    fun `ANDROID AUTO uses badge when phone is unavailable`() {
        val decision = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.ANDROID),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = false,
            nowElapsedMs = now,
        )

        assertEquals(BleInvestigationRoute.BADGE, decision.route)
        assertNull(decision.error)
    }

    @Test
    fun `BADGE AUTO with USB badge path selects badge`() {
        assertEquals(
            BleInvestigationRoute.BADGE,
            selectInvestigationRoute(
                origin = PrivacyDetectionOrigin.BADGE,
                target = gattTarget(PrivacyDetectionOrigin.BADGE),
                badgeAvailable = true,
                requestedRoute = BleInvestigationRoute.AUTO,
                phoneAvailable = true,
                nowElapsedMs = now,
            ).route,
        )
    }

    @Test
    fun `BADGE AUTO ignores legacy bonded badge BLE and falls back to phone`() {
        val availability = BadgeInvestigationAvailability(
            scannerSlotZeroConnected = true,
            usbAvailable = false,
            bleAvailable = true,
            httpAvailable = false,
        )

        assertFalse(availability.badgeAvailable)
        assertEquals(
            BleInvestigationRoute.PHONE,
            selectInvestigationRoute(
                origin = PrivacyDetectionOrigin.BADGE,
                target = gattTarget(PrivacyDetectionOrigin.BADGE),
                badgeAvailable = availability.badgeAvailable,
                requestedRoute = BleInvestigationRoute.AUTO,
                phoneAvailable = true,
                nowElapsedMs = now,
            ).route,
        )
    }

    @Test
    fun `BADGE AUTO falls back to phone only for a fresh valid GATT target`() {
        val fresh = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = gattTarget(PrivacyDetectionOrigin.BADGE),
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )
        val stale = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = gattTarget(PrivacyDetectionOrigin.BADGE).copy(observedAtElapsedMs = 1),
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now + 60_000,
        )
        val malformed = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = gattTarget(PrivacyDetectionOrigin.BADGE).copy(mac = "not-a-mac"),
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertEquals(BleInvestigationRoute.PHONE, fresh.route)
        assertNull(stale.route)
        assertEquals("stale_target", stale.error)
        assertNull(malformed.route)
        assertEquals("invalid_target", malformed.error)
    }

    @Test
    fun `PASSIVE CAPTURE AUTO is badge only`() {
        val available = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = passiveTarget(),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )
        val unavailable = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = passiveTarget(),
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertEquals(BleInvestigationRoute.BADGE, available.route)
        assertNull(unavailable.route)
        assertEquals("badge_unavailable", unavailable.error)
    }

    @Test
    fun `explicit PHONE with stale or missing MAC fails validation`() {
        val stale = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.ANDROID).copy(observedAtElapsedMs = 1),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.PHONE,
            phoneAvailable = true,
            nowElapsedMs = now + 60_000,
        )
        val missing = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.ANDROID).copy(mac = null),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.PHONE,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertEquals("stale_target", stale.error)
        assertEquals("invalid_target", missing.error)
    }

    @Test
    fun `PHONE rejects passive capture even when phone is available`() {
        val decision = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = passiveTarget(),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.PHONE,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertNull(decision.route)
        assertEquals("phone_requires_gatt", decision.error)
    }

    @Test
    fun `structured origin mismatch is rejected`() {
        val decision = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.BADGE),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertNull(decision.route)
        assertEquals("origin_mismatch", decision.error)
    }

    @Test
    fun `explicit unavailable routes fail visibly and never switch`() {
        val phone = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.ANDROID,
            target = gattTarget(PrivacyDetectionOrigin.ANDROID),
            badgeAvailable = true,
            requestedRoute = BleInvestigationRoute.PHONE,
            phoneAvailable = false,
            nowElapsedMs = now,
        )
        val badge = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = gattTarget(PrivacyDetectionOrigin.BADGE),
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.BADGE,
            phoneAvailable = true,
            nowElapsedMs = now,
        )

        assertNull(phone.route)
        assertEquals("phone_unavailable", phone.error)
        assertNull(badge.route)
        assertEquals("badge_unavailable", badge.error)
    }

    @Test
    fun `badge availability requires scanner slot zero and USB`() {
        assertFalse(
            BadgeInvestigationAvailability(
                scannerSlotZeroConnected = false,
                usbAvailable = true,
                bleAvailable = true,
                httpAvailable = true,
            ).badgeAvailable,
        )
        assertFalse(
            BadgeInvestigationAvailability(
                scannerSlotZeroConnected = true,
                usbAvailable = false,
                bleAvailable = false,
                httpAvailable = false,
            ).badgeAvailable,
        )
        assertFalse(
            BadgeInvestigationAvailability(
                scannerSlotZeroConnected = true,
                usbAvailable = false,
                bleAvailable = false,
                httpAvailable = true,
            ).badgeAvailable,
        )
    }

    @Test
    fun `read only HTTP and legacy BLE states never enable badge investigation`() {
        fun state(status: BadgeUsbStatus) = BadgeUsbState(
            status = status,
            controlStatus = BadgeControlStatus(
                scanners = listOf(BadgeScannerStatus(slot = 0, connected = true)),
            ),
        )

        for (status in listOf(
            BadgeUsbStatus.BLE_CONNECTED,
            BadgeUsbStatus.AP_CONNECTED,
            BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
        )) {
            val availability = deriveBadgeInvestigationAvailability(state(status))
            assertFalse(status.name, availability.badgeAvailable)
            assertFalse(status.name, availability.bleAvailable)
            assertFalse(status.name, availability.httpAvailable)
        }

        assertTrue(
            deriveBadgeInvestigationAvailability(state(BadgeUsbStatus.CONNECTED)).badgeAvailable,
        )
    }

    @Test
    fun `badge entity age produces a stale phone fallback target`() {
        val snapshotAtElapsedMs = 70_000L
        val nowElapsedMs = 100_000L
        val entity = BadgeThreatEntity(
            label = "Possible serial skimmer",
            threatClass = "ble",
            category = "SKIM",
            code = "SKIM",
            bssid = "AA:BB:CC:DD:EE:FF",
            score = 80,
            ageSeconds = 45,
            lastSeenSeconds = 1,
            snapshotAtElapsedMs = snapshotAtElapsedMs,
            rssi = -52,
            events = 3,
        )

        val target = entity.toPrivacyDetection(
            now = Instant.EPOCH,
            nowElapsedMs = nowElapsedMs,
        )!!.investigationTarget!!
        val decision = selectInvestigationRoute(
            origin = PrivacyDetectionOrigin.BADGE,
            target = target,
            badgeAvailable = false,
            requestedRoute = BleInvestigationRoute.AUTO,
            phoneAvailable = true,
            nowElapsedMs = nowElapsedMs,
        )

        assertEquals(snapshotAtElapsedMs - 1_000L, target.observedAtElapsedMs)
        assertNull(decision.route)
        assertEquals("stale_target", decision.error)
    }

    @Test
    fun `active investigation rejects a rapid replacement until terminal`() {
        assertTrue(shouldRejectConcurrentInvestigationStart(hasActive = true, BleInvestigationState.QUEUED))
        assertTrue(shouldRejectConcurrentInvestigationStart(hasActive = true, BleInvestigationState.READING))
        assertFalse(shouldRejectConcurrentInvestigationStart(hasActive = true, BleInvestigationState.COMPLETE))
        assertFalse(shouldRejectConcurrentInvestigationStart(hasActive = true, BleInvestigationState.FAILED))
        assertFalse(shouldRejectConcurrentInvestigationStart(hasActive = false, null))
    }

    private fun gattTarget(origin: PrivacyDetectionOrigin) = BleInvestigationTarget(
        mode = BleInvestigationMode.GATT,
        mac = "AA:BB:CC:DD:EE:FF",
        entityKey = "entity",
        observedAtElapsedMs = now - 1,
        origin = origin,
    )

    private fun passiveTarget() = BleInvestigationTarget(
        mode = BleInvestigationMode.PASSIVE_CAPTURE,
        mac = null,
        entityKey = "pairing-spam",
        observedAtElapsedMs = now - 1,
        origin = PrivacyDetectionOrigin.BADGE,
    )
}
