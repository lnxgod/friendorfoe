package com.friendorfoe.presentation.privacy

fun capabilitiesFor(
    source: PrivacySourceKind,
    stableId: String?,
    hasLiveLocalSamples: Boolean,
    freshness: FindingFreshness,
    sourceHealth: SourceHealthState,
): PrivacyCapabilities {
    val canIgnore = !stableId.isNullOrBlank()
    val canTrack = source == PrivacySourceKind.PHONE_BLE &&
        canIgnore &&
        hasLiveLocalSamples &&
        freshness == FindingFreshness.LIVE &&
        sourceHealth == SourceHealthState.LIVE
    return PrivacyCapabilities(
        canIgnore = canIgnore,
        canTrack = canTrack,
        canOpenDirectionSweep = canTrack,
    )
}

class PrivacyCurrentReducer {
    fun reduce(
        sources: List<PrivacySourceSnapshot>,
        ignoredKeys: Set<String>,
        nowElapsedMs: Long,
    ): PrivacyCurrentState {
        val effectiveSources = sources
            .map { snapshot ->
                snapshot.copy(health = agedSourceHealth(snapshot.health, nowElapsedMs))
            }
            .sortedBy { it.health.source.preferenceId }

        val rows = effectiveSources
            .flatMap { snapshot ->
                snapshot.findings.mapNotNull { raw ->
                    val normalized = PrivacyFindingNormalizer.normalize(raw)
                    val freshness = freshnessFor(
                        source = normalized.source,
                        seenAt = normalized.lastObservedElapsedMs,
                        now = nowElapsedMs,
                        protocolTtlMs = normalized.protocolTtlMs,
                        sourceHealth = snapshot.health.state,
                    )
                    normalized.copy(
                        freshness = freshness,
                        capabilities = capabilitiesFor(
                            source = normalized.source,
                            stableId = normalized.stableSourceId,
                            hasLiveLocalSamples = normalized.hasLiveLocalSamples,
                            freshness = freshness,
                            sourceHealth = snapshot.health.state,
                        ),
                    )
                        .takeUnless { freshness == FindingFreshness.EXPIRED }
                        ?.takeUnless { it.ignoreKey?.encoded in ignoredKeys }
                }
            }
            .groupBy(PrivacyFinding::observationKey)
            .map { (_, duplicates) ->
                duplicates.maxWithOrNull(privacyFindingDuplicateComparator)
                    ?: error("A grouped observation must contain at least one finding")
            }
            .sortedWith(
                compareByDescending<PrivacyFinding> { it.severity.rank }
                    .thenByDescending { it.lastObservedElapsedMs }
                    .thenBy { it.source.preferenceId }
                    .thenBy { it.observationKey.encoded },
            )

        val liveThreats = rows.filter { finding ->
            finding.freshness == FindingFreshness.LIVE &&
                finding.ownership != Ownership.OWNED &&
                finding.severity.rank >= FindingSeverity.AWARENESS.rank
        }
        val alertEligible = liveThreats.filter { finding ->
            finding.severity == FindingSeverity.CRITICAL && finding.routableKey != null
        }

        return PrivacyCurrentState(
            sources = effectiveSources.map(PrivacySourceSnapshot::health),
            findings = rows,
            threatCount = liveThreats.size,
            alertEligible = alertEligible,
            initialResolutionComplete = effectiveSources.isNotEmpty() &&
                effectiveSources.none { it.health.state == SourceHealthState.LOADING },
        )
    }
}

private val privacyFindingDuplicateComparator =
    compareBy<PrivacyFinding> { it.lastObservedElapsedMs }
        .thenBy { it.title }
        .thenBy { it.evidence }
        .thenBy { it.limitation }
        .thenBy { it.severity.rank }
        .thenBy { it.ownership.ordinal }
        .thenBy { it.routableKey?.encoded }
        .thenBy { it.stableSourceId }
        .thenBy { it.category.ordinal }
        .thenBy { it.signalDbm }
        .thenBy { it.firstSeenWallMs }
        .thenBy { it.lastSeenWallMs }
        .thenBy { it.protocolTtlMs }
        .thenBy { it.hasLiveLocalSamples }
        .thenBy { it.appleEvidence != null }
        .thenBy { it.appleEvidence?.appleFamilyEvidence }
        .thenBy { it.appleEvidence?.airPodsAssociationEvidence }
        .thenBy { it.appleEvidence?.listeningOrientedCategoryOrWording }
        .thenBy { it.capabilities.canIgnore }
        .thenBy { it.capabilities.canTrack }
        .thenBy { it.capabilities.canOpenDirectionSweep }
        .thenBy { it.freshness.ordinal }
        .thenBy { it.displayId }
        .thenBy { it.source.preferenceId }
        .thenBy { it.observationKey.encoded }
