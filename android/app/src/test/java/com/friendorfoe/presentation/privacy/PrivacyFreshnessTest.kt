package com.friendorfoe.presentation.privacy

import org.junit.Assert.assertEquals
import org.junit.Test

class PrivacyFreshnessTest {
    private val now = 100_000L

    @Test
    fun everySourceUsesItsApprovedStaleBoundary() {
        val cases = listOf(
            PrivacySourceKind.PHONE_BLE to 30_000L,
            PrivacySourceKind.PHONE_ULTRASONIC to 30_000L,
            PrivacySourceKind.BACKEND to 15_000L,
            PrivacySourceKind.BADGE_USB to 10_000L,
            PrivacySourceKind.BADGE_AP to 10_000L,
            PrivacySourceKind.BADGE_BLE to 20_000L,
            PrivacySourceKind.BADGE_DEBUG_BRIDGE to 10_000L,
            PrivacySourceKind.WIFI_ANALYSIS to 30_000L,
        )

        cases.forEach { (source, staleAfterMs) ->
            assertEquals(
                "$source remains live immediately before its stale boundary",
                FindingFreshness.LIVE,
                freshnessFor(source, now - staleAfterMs + 1L, now),
            )
            assertEquals(
                "$source becomes stale at its exact stale boundary",
                FindingFreshness.STALE,
                freshnessFor(source, now - staleAfterMs, now),
            )
        }
    }

    @Test
    fun everySourceExpiresAtItsApprovedRemovalBoundary() {
        val cases = listOf(
            PrivacySourceKind.PHONE_BLE to 90_000L,
            PrivacySourceKind.PHONE_ULTRASONIC to 60_000L,
            PrivacySourceKind.BACKEND to 60_000L,
            PrivacySourceKind.BADGE_USB to 60_000L,
            PrivacySourceKind.BADGE_AP to 60_000L,
            PrivacySourceKind.BADGE_BLE to 60_000L,
            PrivacySourceKind.BADGE_DEBUG_BRIDGE to 60_000L,
            PrivacySourceKind.WIFI_ANALYSIS to 60_000L,
        )

        cases.forEach { (source, removeAfterMs) ->
            assertEquals(
                "$source is retained immediately before removal",
                FindingFreshness.STALE,
                freshnessFor(source, now - removeAfterMs + 1L, now),
            )
            assertEquals(
                "$source expires at its exact removal boundary",
                FindingFreshness.EXPIRED,
                freshnessFor(source, now - removeAfterMs, now),
            )
        }
    }

    @Test
    fun shorterProtocolTtlCanShortenButNeverExtendLifetime() {
        assertEquals(
            FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.BACKEND, 85_000L, now, protocolTtlMs = 20_000L),
        )
        assertEquals(
            FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.BACKEND, 80_000L, now, protocolTtlMs = 20_000L),
        )
        assertEquals(
            FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.BACKEND, 40_000L, now, protocolTtlMs = 120_000L),
        )
    }

    @Test
    fun protocolTtlShorterThanTheStaleWindowExpiresWithoutExtendingTheRow() {
        assertEquals(
            FindingFreshness.LIVE,
            freshnessFor(PrivacySourceKind.PHONE_BLE, 95_001L, now, protocolTtlMs = 5_000L),
        )
        assertEquals(
            FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.PHONE_BLE, 95_000L, now, protocolTtlMs = 5_000L),
        )
    }

    @Test
    fun pausedRowsKeepAgingUntilTheyExpire() {
        assertEquals(
            FindingFreshness.PAUSED_CACHED,
            freshnessFor(
                PrivacySourceKind.PHONE_BLE,
                seenAt = 95_000L,
                now = now,
                sourceHealth = SourceHealthState.PAUSED,
            ),
        )
        assertEquals(
            FindingFreshness.PAUSED_CACHED,
            freshnessFor(
                PrivacySourceKind.PHONE_BLE,
                seenAt = 10_001L,
                now = now,
                sourceHealth = SourceHealthState.PAUSED,
            ),
        )
        assertEquals(
            FindingFreshness.EXPIRED,
            freshnessFor(
                PrivacySourceKind.PHONE_BLE,
                seenAt = 10_000L,
                now = now,
                sourceHealth = SourceHealthState.PAUSED,
            ),
        )
    }

    @Test
    fun nonLiveSourceHealthMakesRetainedRowsStale() {
        listOf(
            SourceHealthState.LOADING,
            SourceHealthState.STALE,
            SourceHealthState.PERMISSION_BLOCKED,
            SourceHealthState.UNSUPPORTED,
            SourceHealthState.FAILED,
        ).forEach { health ->
            assertEquals(
                health.name,
                FindingFreshness.STALE,
                freshnessFor(
                    PrivacySourceKind.BACKEND,
                    seenAt = 99_999L,
                    now = now,
                    sourceHealth = health,
                ),
            )
        }
    }

    @Test
    fun futureTimestampsDoNotCreateNegativeAges() {
        assertEquals(
            FindingFreshness.LIVE,
            freshnessFor(PrivacySourceKind.PHONE_BLE, seenAt = Long.MAX_VALUE, now = 0L),
        )
    }

    @Test
    fun elapsedAgeArithmeticSaturatesInsteadOfOverflowing() {
        assertEquals(
            FindingFreshness.EXPIRED,
            freshnessFor(
                PrivacySourceKind.PHONE_BLE,
                seenAt = Long.MIN_VALUE,
                now = Long.MAX_VALUE,
            ),
        )
        val health = liveHealth(
            source = PrivacySourceKind.BACKEND,
            lastSuccessElapsedMs = Long.MIN_VALUE,
        )
        assertEquals(
            SourceHealthState.STALE,
            agedSourceHealth(health, Long.MAX_VALUE).state,
        )
    }

    @Test
    fun liveSourceHealthAgesAtTheExactStaleBoundary() {
        val health = liveHealth(
            source = PrivacySourceKind.BADGE_USB,
            lastSuccessElapsedMs = 90_000L,
        )

        assertEquals(SourceHealthState.LIVE, agedSourceHealth(health, 99_999L).state)
        assertEquals(SourceHealthState.STALE, agedSourceHealth(health, now).state)
    }

    @Test
    fun liveSourceWithoutASuccessTimestampReturnsToLoading() {
        val health = liveHealth(
            source = PrivacySourceKind.PHONE_BLE,
            lastSuccessElapsedMs = null,
        )

        assertEquals(SourceHealthState.LOADING, agedSourceHealth(health, now).state)
    }

    @Test
    fun resumingWithCachedRowsDoesNotRejuvenateThem() {
        assertEquals(SourceHealthState.STALE, resumedHealth(hasRetainedRows = true))
        assertEquals(SourceHealthState.LOADING, resumedHealth(hasRetainedRows = false))
    }

    private fun liveHealth(
        source: PrivacySourceKind,
        lastSuccessElapsedMs: Long?,
    ) = PrivacySourceHealth(
        source = source,
        state = SourceHealthState.LIVE,
        lastSuccessElapsedMs = lastSuccessElapsedMs,
        lastSuccessWallMs = null,
        recoveryLabel = null,
        message = null,
    )
}
