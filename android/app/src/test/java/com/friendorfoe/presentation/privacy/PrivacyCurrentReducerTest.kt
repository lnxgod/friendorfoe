package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyCurrentReducerTest {
    private val now = 100_000L

    @Test
    fun sameRawIdentifierFromPhoneBadgeAndWifiNeverCollapses() {
        val state = reduce(
            liveSnapshot(finding(PrivacySourceKind.PHONE_BLE, "shared", title = "Phone")),
            liveSnapshot(finding(PrivacySourceKind.BADGE_USB, "shared", title = "Badge")),
            liveSnapshot(finding(PrivacySourceKind.WIFI_ANALYSIS, "shared", title = "Wi-Fi")),
        )

        assertEquals(listOf("Badge", "Phone", "Wi-Fi"), state.findings.map { it.title }.sorted())
    }

    @Test
    fun ignoreKeySuppressesOnlyTheExactSourceAndStableIdentity() {
        val phone = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "phone-observation",
            stableSourceId = "shared-stable-id",
        )
        val badge = finding(
            source = PrivacySourceKind.BADGE_USB,
            sourceRecordId = "badge-observation",
            stableSourceId = "shared-stable-id",
        )
        val phoneKey = FindingPreferenceKey.create("phone_ble", "shared-stable-id")
        assertNotNull(phoneKey)

        val state = PrivacyCurrentReducer().reduce(
            sources = listOf(liveSnapshot(phone), liveSnapshot(badge)),
            ignoredKeys = setOf(requireNotNull(phoneKey).encoded),
            nowElapsedMs = now,
        )

        assertEquals(listOf(PrivacySourceKind.BADGE_USB), state.findings.map { it.source })
    }

    @Test
    fun preferenceIgnoreKeyUsesExactAppPreferencesEncoding() {
        val finding = finding(
            source = PrivacySourceKind.BADGE_DEBUG_BRIDGE,
            sourceRecordId = "observation:9",
            stableSourceId = "entity:9",
        )
        val expected = FindingPreferenceKey.create("badge_debug_bridge", "entity:9")

        assertEquals(expected, finding.ignoreKey)
        assertEquals("badge_debug_bridge\u001Fentity:9", finding.ignoreKey?.encoded)
    }

    @Test
    fun stableIgnoreAndRoutableIdentitiesAreIndependent() {
        val ignorableOnly = finding(
            source = PrivacySourceKind.BADGE_USB,
            sourceRecordId = "ignore-observation",
            stableSourceId = "stable",
            routableKey = null,
            severity = FindingSeverity.CRITICAL,
        )
        val routableOnly = finding(
            source = PrivacySourceKind.BACKEND,
            sourceRecordId = "route-observation",
            stableSourceId = null,
            routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "backend-route"),
            severity = FindingSeverity.CRITICAL,
        )

        val state = reduce(liveSnapshot(ignorableOnly), liveSnapshot(routableOnly))

        assertNotNull(state.findings.single { it.title == ignorableOnly.title }.ignoreKey)
        assertNull(state.findings.single { it.title == routableOnly.title }.ignoreKey)
        assertFalse(state.alertEligible.contains(ignorableOnly))
        assertEquals(listOf("BACKEND route-observation"), state.alertEligible.map { it.title })
    }

    @Test
    fun threatCountIncludesOnlyLiveUnownedAwarenessOrCriticalRows() {
        val state = reduce(
            liveSnapshot(finding(PrivacySourceKind.PHONE_BLE, "critical", FindingSeverity.CRITICAL)),
            liveSnapshot(finding(PrivacySourceKind.BACKEND, "awareness", FindingSeverity.AWARENESS)),
            liveSnapshot(
                finding(
                    PrivacySourceKind.BADGE_USB,
                    "owned",
                    FindingSeverity.CRITICAL,
                    ownership = Ownership.OWNED,
                )
            ),
            staleSnapshot(finding(PrivacySourceKind.BADGE_BLE, "stale", FindingSeverity.CRITICAL)),
            pausedSnapshot(finding(PrivacySourceKind.BADGE_AP, "paused", FindingSeverity.CRITICAL)),
            liveSnapshot(finding(PrivacySourceKind.WIFI_ANALYSIS, "nearby", FindingSeverity.NEARBY)),
            liveSnapshot(finding(PrivacySourceKind.PHONE_ULTRASONIC, "info", FindingSeverity.INFO)),
        )

        assertEquals(2, state.threatCount)
    }

    @Test
    fun alertsRequireLiveUnownedCriticalAndRoutableRows() {
        val eligible = finding(
            PrivacySourceKind.BACKEND,
            "eligible",
            FindingSeverity.CRITICAL,
            routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "route-eligible"),
        )
        val awareness = finding(
            PrivacySourceKind.PHONE_BLE,
            "awareness",
            FindingSeverity.AWARENESS,
            routableKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "route-awareness"),
        )
        val noRoute = finding(
            PrivacySourceKind.BADGE_USB,
            "no-route",
            FindingSeverity.CRITICAL,
            routableKey = null,
        )
        val owned = finding(
            PrivacySourceKind.WIFI_ANALYSIS,
            "owned",
            FindingSeverity.CRITICAL,
            ownership = Ownership.OWNED,
            routableKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, "route-owned"),
        )
        val stale = finding(
            PrivacySourceKind.BADGE_BLE,
            "stale",
            FindingSeverity.CRITICAL,
            routableKey = PrivacyFindingKey(PrivacySourceKind.BADGE_BLE, "route-stale"),
        )

        val state = reduce(
            liveSnapshot(eligible),
            liveSnapshot(awareness),
            liveSnapshot(noRoute),
            liveSnapshot(owned),
            staleSnapshot(stale),
        )

        assertEquals(listOf("BACKEND eligible"), state.alertEligible.map { it.title })
    }

    @Test
    fun duplicateObservationWithinOneSourceKeepsTheNewestDeterministically() {
        val older = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "same-observation",
            title = "Older",
            lastObservedElapsedMs = 99_000L,
        )
        val newer = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "same-observation",
            title = "Newer",
            lastObservedElapsedMs = 99_500L,
        )

        val state = reduce(liveSnapshot(older, newer))

        assertEquals(listOf("Newer"), state.findings.map { it.title })
    }

    @Test
    fun duplicateTieBreakDoesNotDependOnInputOrder() {
        val alpha = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "same-observation",
            title = "Alpha",
        )
        val omega = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "same-observation",
            title = "Omega",
        )

        val forwards = reduce(liveSnapshot(alpha, omega)).findings.single().title
        val backwards = reduce(liveSnapshot(omega, alpha)).findings.single().title

        assertEquals("Omega", forwards)
        assertEquals(forwards, backwards)
    }

    @Test
    fun contradictoryDuplicateTieBreakIsTotalAndOrderIndependent() {
        val base = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "same-observation",
            title = "Same visible row",
            severity = FindingSeverity.INFO,
        )
        val informationalOwned = base.copy(ownership = Ownership.OWNED)
        val criticalRoutable = base.copy(
            severity = FindingSeverity.CRITICAL,
            ownership = Ownership.UNKNOWN,
            routableKey = PrivacyFindingKey(
                PrivacySourceKind.PHONE_BLE,
                "route:same-observation",
            ),
        )

        val forwards = reduce(liveSnapshot(informationalOwned, criticalRoutable))
        val backwards = reduce(liveSnapshot(criticalRoutable, informationalOwned))

        assertEquals(forwards, backwards)
        assertEquals(FindingSeverity.CRITICAL, forwards.findings.single().severity)
        assertEquals(1, forwards.threatCount)
        assertEquals(listOf(criticalRoutable.routableKey), forwards.alertEligible.map { it.routableKey })
    }

    @Test
    fun displayIdIsNeverUsedAsReducerIdentity() {
        val first = finding(
            PrivacySourceKind.PHONE_BLE,
            "first-observation",
            displayId = "same-display-id",
        )
        val second = finding(
            PrivacySourceKind.PHONE_BLE,
            "second-observation",
            displayId = "same-display-id",
        )

        assertEquals(2, reduce(liveSnapshot(first, second)).findings.size)
    }

    @Test
    fun snapshotRejectsFindingFromAnotherSource() {
        val failure = runCatching {
            PrivacySourceSnapshot(
                health = health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE),
                findings = listOf(finding(PrivacySourceKind.WIFI_ANALYSIS, "wifi-row")),
                emittedAtElapsedMs = now,
            )
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
    }

    @Test
    fun findingRejectsObservationIdentityFromAnotherSource() {
        val failure = runCatching {
            finding(
                source = PrivacySourceKind.PHONE_BLE,
                sourceRecordId = "phone-row",
                observationKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, "wifi-row"),
            )
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
    }

    @Test
    fun findingRejectsRoutableIdentityFromAnotherSource() {
        val failure = runCatching {
            finding(
                source = PrivacySourceKind.PHONE_BLE,
                sourceRecordId = "phone-row",
                routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "backend-route"),
            )
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
    }

    @Test
    fun blankSourceRecordIdentityIsRejected() {
        val failure = runCatching {
            PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "   ")
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
    }

    @Test
    fun sourcesAndFindingsHaveDeterministicSortOrder() {
        val rows = listOf(
            finding(
                PrivacySourceKind.WIFI_ANALYSIS,
                "wifi-critical-old",
                FindingSeverity.CRITICAL,
                lastObservedElapsedMs = 98_000L,
            ),
            finding(
                PrivacySourceKind.BACKEND,
                "backend-critical-new",
                FindingSeverity.CRITICAL,
                lastObservedElapsedMs = 99_000L,
            ),
            finding(
                PrivacySourceKind.PHONE_BLE,
                "phone-awareness",
                FindingSeverity.AWARENESS,
                lastObservedElapsedMs = 99_900L,
            ),
        )
        val input = listOf(
            liveSnapshot(rows[0]),
            liveSnapshot(rows[2]),
            liveSnapshot(rows[1]),
        )

        val forwards = PrivacyCurrentReducer().reduce(input, emptySet(), now)
        val backwards = PrivacyCurrentReducer().reduce(input.reversed(), emptySet(), now)

        assertEquals(
            listOf("backend", "phone_ble", "wifi_analysis"),
            forwards.sources.map { it.source.preferenceId },
        )
        assertEquals(
            listOf("BACKEND backend-critical-new", "WIFI_ANALYSIS wifi-critical-old", "PHONE_BLE phone-awareness"),
            forwards.findings.map { it.title },
        )
        assertEquals(forwards, backwards)
    }

    @Test
    fun expiredRowsLeaveCurrentRegardlessOfSourceHealth() {
        val expired = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "expired",
            lastObservedElapsedMs = 10_000L,
        )

        assertTrue(reduce(pausedSnapshot(expired)).findings.isEmpty())
    }

    @Test
    fun sourceHealthIsAgedBeforeCapabilitiesAndRows() {
        val row = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "phone",
            stableSourceId = "fp:phone",
            hasLiveLocalSamples = true,
            lastObservedElapsedMs = 99_999L,
        )
        val staleHealth = health(
            source = PrivacySourceKind.PHONE_BLE,
            state = SourceHealthState.LIVE,
            lastSuccessElapsedMs = 70_000L,
        )

        val state = PrivacyCurrentReducer().reduce(
            sources = listOf(PrivacySourceSnapshot(staleHealth, listOf(row), now)),
            ignoredKeys = emptySet(),
            nowElapsedMs = now,
        )

        assertEquals(SourceHealthState.STALE, state.sources.single().state)
        assertEquals(FindingFreshness.STALE, state.findings.single().freshness)
        assertEquals(PrivacyCapabilities(canIgnore = true), state.findings.single().capabilities)
    }

    @Test
    fun appleListeningNormalizationRunsBeforeThreatAndAlertSelection() {
        val apple = finding(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "apple-listening",
            title = "Possible iPhone listening alert",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            routableKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "route:apple"),
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = true,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        val state = reduce(liveSnapshot(apple))

        assertEquals(FindingSeverity.INFO, state.findings.single().severity)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, state.findings.single().category)
        assertEquals(0, state.threatCount)
        assertTrue(state.alertEligible.isEmpty())
    }

    @Test
    fun legacySameRowAppleListeningClaimCannotBecomeAThreatOrAlert() {
        val legacy = finding(
            source = PrivacySourceKind.BACKEND,
            sourceRecordId = "legacy-apple-listening",
            title = "Apple AirPods possible eavesdrop listening",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "route:legacy-apple"),
            appleEvidence = null,
        )

        val state = reduce(liveSnapshot(legacy))

        assertEquals(FindingSeverity.INFO, state.findings.single().severity)
        assertEquals(0, state.threatCount)
        assertTrue(state.alertEligible.isEmpty())
    }

    @Test
    fun initialResolutionRequiresAtLeastOneSourceAndNoLoadingHealth() {
        val empty = PrivacyCurrentReducer().reduce(emptyList(), emptySet(), now)
        val loading = reduce(
            snapshot(
                source = PrivacySourceKind.PHONE_BLE,
                state = SourceHealthState.LOADING,
                findings = emptyList(),
            )
        )
        val resolved = reduce(
            snapshot(
                source = PrivacySourceKind.PHONE_BLE,
                state = SourceHealthState.PAUSED,
                findings = emptyList(),
            ),
            snapshot(
                source = PrivacySourceKind.BACKEND,
                state = SourceHealthState.FAILED,
                findings = emptyList(),
            ),
        )

        assertFalse(empty.initialResolutionComplete)
        assertFalse(loading.initialResolutionComplete)
        assertTrue(resolved.initialResolutionComplete)
    }

    private fun reduce(vararg snapshots: PrivacySourceSnapshot): PrivacyCurrentState =
        PrivacyCurrentReducer().reduce(snapshots.toList(), emptySet(), now)

    private fun liveSnapshot(vararg findings: PrivacyFinding) = snapshot(
        source = findings.first().source,
        state = SourceHealthState.LIVE,
        findings = findings.toList(),
    )

    private fun staleSnapshot(finding: PrivacyFinding) = snapshot(
        source = finding.source,
        state = SourceHealthState.STALE,
        findings = listOf(finding),
    )

    private fun pausedSnapshot(finding: PrivacyFinding) = snapshot(
        source = finding.source,
        state = SourceHealthState.PAUSED,
        findings = listOf(finding),
    )

    private fun snapshot(
        source: PrivacySourceKind,
        state: SourceHealthState,
        findings: List<PrivacyFinding>,
    ) = PrivacySourceSnapshot(
        health = health(source, state),
        findings = findings,
        emittedAtElapsedMs = now,
    )

    private fun health(
        source: PrivacySourceKind,
        state: SourceHealthState,
        lastSuccessElapsedMs: Long? = if (state == SourceHealthState.LIVE) now else 99_000L,
    ) = PrivacySourceHealth(
        source = source,
        state = state,
        lastSuccessElapsedMs = lastSuccessElapsedMs,
        lastSuccessWallMs = null,
        recoveryLabel = null,
        message = null,
    )

    private fun finding(
        source: PrivacySourceKind,
        sourceRecordId: String,
        severity: FindingSeverity = FindingSeverity.INFO,
        title: String = "$source $sourceRecordId",
        displayId: String = "display:$sourceRecordId",
        stableSourceId: String? = "stable:$sourceRecordId",
        routableKey: PrivacyFindingKey? = null,
        ownership: Ownership = Ownership.UNKNOWN,
        lastObservedElapsedMs: Long = 99_000L,
        hasLiveLocalSamples: Boolean = source == PrivacySourceKind.PHONE_BLE,
        observationKey: PrivacyFindingKey = PrivacyFindingKey(source, sourceRecordId),
        category: PrivacyCategory = PrivacyCategory.INFORMATIONAL,
        appleEvidence: PrivacyAppleListeningEvidence? = null,
    ) = PrivacyFinding(
        displayId = displayId,
        observationKey = observationKey,
        source = source,
        stableSourceId = stableSourceId,
        routableKey = routableKey,
        title = title,
        evidence = null,
        limitation = null,
        category = category,
        severity = severity,
        ownership = ownership,
        signalDbm = null,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = lastObservedElapsedMs,
        protocolTtlMs = null,
        hasLiveLocalSamples = hasLiveLocalSamples,
        appleEvidence = appleEvidence,
    )
}
