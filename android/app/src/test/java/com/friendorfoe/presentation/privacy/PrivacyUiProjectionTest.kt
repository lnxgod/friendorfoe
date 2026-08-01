package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyUiProjectionTest {

    @Test
    fun findingCountIsQualifiedUntilEverySourceFinishesInitialResolution() {
        val unresolvedEmpty = projectPrivacyUiState(
            current(initialResolutionComplete = false),
        )
        val unresolvedFinding = projectPrivacyUiState(
            current(
                findings = listOf(finding()),
                initialResolutionComplete = false,
            ),
        )
        val resolvedEmpty = projectPrivacyUiState(current())
        val resolvedFinding = projectPrivacyUiState(current(findings = listOf(finding())))

        assertFalse(unresolvedEmpty.initialResolutionComplete)
        assertEquals("0 findings so far · sources still resolving", unresolvedEmpty.findingCountLabel)
        assertEquals("1 finding so far · some sources still resolving", unresolvedFinding.findingCountLabel)
        assertTrue(resolvedEmpty.initialResolutionComplete)
        assertEquals("0 current findings", resolvedEmpty.findingCountLabel)
        assertEquals("1 current finding", resolvedFinding.findingCountLabel)
    }
    @Test
    fun findingsMapToTheFourHumanReadableSections() {
        assertEquals(PrivacySection.THREATS, finding(FindingSeverity.CRITICAL).section())
        assertEquals(PrivacySection.AWARENESS, finding(FindingSeverity.AWARENESS).section())
        assertEquals(PrivacySection.NEARBY, finding(FindingSeverity.NEARBY).section())
        assertEquals(PrivacySection.INFO, finding(FindingSeverity.INFO).section())
    }

    @Test
    fun filterCountCountsEachFilterFamilyOnce() {
        val filters = PrivacyFilterState(
            query = "camera",
            categories = setOf(PrivacyCategory.HIDDEN_CAMERA, PrivacyCategory.BODY_CAMERA),
            sources = setOf(PrivacySourceKind.PHONE_BLE, PrivacySourceKind.BACKEND),
        )

        assertEquals(3, filters.activeFilterCount)
    }

    @Test
    fun filteringUsesPlainLanguageFieldsAndKeepsReducerOrder() {
        val critical = finding(
            severity = FindingSeverity.CRITICAL,
            source = PrivacySourceKind.BACKEND,
            title = "Hidden camera",
            evidence = "Repeated network observation",
            id = "critical",
        )
        val nearby = finding(
            severity = FindingSeverity.NEARBY,
            source = PrivacySourceKind.PHONE_BLE,
            title = "BLE tracker",
            evidence = "Nearby signal",
            id = "nearby",
        )
        val state = current(findings = listOf(critical, nearby))

        assertEquals(
            listOf("critical"),
            projectPrivacyUiState(state, PrivacyFilterState(query = "network"))
                .visibleFindings.map(PrivacyFinding::displayId),
        )
        assertEquals(
            listOf("nearby"),
            projectPrivacyUiState(
                state,
                PrivacyFilterState(sources = setOf(PrivacySourceKind.PHONE_BLE)),
            ).visibleFindings.map(PrivacyFinding::displayId),
        )
    }

    @Test
    fun retainedRowsStayContentWhenOneSourceFails() {
        val state = current(
            findings = listOf(finding()),
            sources = listOf(
                health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE, wallMs = 900),
                health(PrivacySourceKind.BACKEND, SourceHealthState.FAILED, wallMs = 800),
            ),
        )

        val projected = projectPrivacyUiState(state)

        assertEquals(PrivacyBodyState.Content, projected.body)
        assertEquals(1, projected.partialFailureCount)
        assertEquals(900L, projected.lastUpdatedWallMs)
    }

    @Test
    fun activeFiltersWithNoResultsAreNotPresentedAsAnEmptyScan() {
        val projected = projectPrivacyUiState(
            current(findings = listOf(finding(title = "BLE tracker"))),
            PrivacyFilterState(query = "camera"),
        )

        assertEquals(PrivacyBodyState.NoMatches(activeFilterCount = 1), projected.body)
    }

    @Test
    fun terminalEmptyStatesKeepTheirTruthfulRecovery() {
        assertEquals(
            PrivacyBodyState.RetryableFailure("Backend failed"),
            projectPrivacyUiState(
                current(sources = listOf(
                    health(
                        PrivacySourceKind.BACKEND,
                        SourceHealthState.FAILED,
                        message = "Backend failed",
                    ),
                )),
            ).body,
        )
        assertEquals(
            PrivacyBodyState.PermissionBlocked("Bluetooth permission is needed"),
            projectPrivacyUiState(
                current(sources = listOf(
                    health(
                        PrivacySourceKind.PHONE_BLE,
                        SourceHealthState.PERMISSION_BLOCKED,
                        message = "Bluetooth permission is needed",
                    ),
                )),
            ).body,
        )
        assertEquals(
            PrivacyBodyState.Empty,
            projectPrivacyUiState(
                current(sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE))),
            ).body,
        )
        assertEquals(
            PrivacyBodyState.Stale("No privacy source has reported fresh results."),
            projectPrivacyUiState(
                current(sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.STALE))),
            ).body,
        )
    }

    @Test
    fun phoneRollupIgnoresDisabledOptionalUltrasonicButKeepsPermissionFailure() {
        assertEquals(
            SourceHealthState.LIVE,
            summarizePrivacySources(
                listOf(
                    health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE),
                    health(PrivacySourceKind.PHONE_ULTRASONIC, SourceHealthState.PAUSED),
                ),
            ).single().state,
        )
        assertEquals(
            SourceHealthState.PERMISSION_BLOCKED,
            summarizePrivacySources(
                listOf(
                    health(PrivacySourceKind.PHONE_BLE, SourceHealthState.PERMISSION_BLOCKED),
                    health(PrivacySourceKind.PHONE_ULTRASONIC, SourceHealthState.LIVE),
                ),
            ).single().state,
        )
        assertEquals(
            SourceHealthState.PAUSED,
            summarizePrivacySources(
                listOf(
                    health(PrivacySourceKind.PHONE_BLE, SourceHealthState.PAUSED),
                    health(PrivacySourceKind.PHONE_ULTRASONIC, SourceHealthState.PAUSED),
                ),
            ).single().state,
        )
    }

    @Test
    fun focusedFindingUsesOnlyTheExactRoutableKey() {
        val expected = finding(id = "expected", routableId = "entity:42")
        val lookalike = finding(
            id = "lookalike",
            source = PrivacySourceKind.BADGE_USB,
            routableId = "entity:42",
        )
        val target = requireNotNull(expected.routableKey)

        val projected = projectPrivacyUiState(
            current(findings = listOf(lookalike, expected)),
            focusedKey = target,
        )

        assertEquals("expected", projected.focusedFinding?.displayId)
        assertNull(projectPrivacyUiState(current(), focusedKey = target).focusedFinding)
    }

    private fun current(
        findings: List<PrivacyFinding> = emptyList(),
        sources: List<PrivacySourceHealth> = listOf(
            health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE),
        ),
        initialResolutionComplete: Boolean = true,
    ) = PrivacyCurrentState(
        sources = sources,
        findings = findings,
        threatCount = findings.count { it.severity.rank >= FindingSeverity.AWARENESS.rank },
        alertEligible = emptyList(),
        initialResolutionComplete = initialResolutionComplete,
    )

    private fun health(
        source: PrivacySourceKind,
        state: SourceHealthState,
        wallMs: Long? = null,
        message: String? = null,
    ) = PrivacySourceHealth(
        source = source,
        state = state,
        lastSuccessElapsedMs = wallMs,
        lastSuccessWallMs = wallMs,
        recoveryLabel = null,
        message = message,
    )

    private fun finding(
        severity: FindingSeverity = FindingSeverity.NEARBY,
        source: PrivacySourceKind = PrivacySourceKind.PHONE_BLE,
        title: String = "BLE tracker",
        evidence: String? = "Nearby signal",
        id: String = "finding",
        routableId: String? = null,
    ) = PrivacyFinding(
        displayId = id,
        observationKey = PrivacyFindingKey(source, "observation:$id"),
        source = source,
        stableSourceId = "stable:$id",
        routableKey = routableId?.let { PrivacyFindingKey(source, it) },
        title = title,
        evidence = evidence,
        limitation = null,
        category = PrivacyCategory.BLE_TRACKER,
        severity = severity,
        ownership = Ownership.UNKNOWN,
        signalDbm = -62,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 1_000,
        protocolTtlMs = null,
        hasLiveLocalSamples = source == PrivacySourceKind.PHONE_BLE,
    )
}
