package com.friendorfoe.presentation.privacy

data class FreshnessPolicy(
    val staleAfterMs: Long,
    val removeAfterMs: Long,
)

val PrivacyFreshnessPolicies: Map<PrivacySourceKind, FreshnessPolicy> = mapOf(
    PrivacySourceKind.PHONE_BLE to FreshnessPolicy(staleAfterMs = 30_000L, removeAfterMs = 90_000L),
    PrivacySourceKind.PHONE_ULTRASONIC to FreshnessPolicy(staleAfterMs = 30_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.BACKEND to FreshnessPolicy(staleAfterMs = 15_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.BADGE_USB to FreshnessPolicy(staleAfterMs = 10_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.BADGE_AP to FreshnessPolicy(staleAfterMs = 10_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.BADGE_BLE to FreshnessPolicy(staleAfterMs = 20_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.BADGE_DEBUG_BRIDGE to FreshnessPolicy(staleAfterMs = 10_000L, removeAfterMs = 60_000L),
    PrivacySourceKind.WIFI_ANALYSIS to FreshnessPolicy(staleAfterMs = 30_000L, removeAfterMs = 60_000L),
)

fun freshnessFor(
    source: PrivacySourceKind,
    seenAt: Long,
    now: Long,
    protocolTtlMs: Long? = null,
    sourceHealth: SourceHealthState = SourceHealthState.LIVE,
): FindingFreshness {
    val policy = PrivacyFreshnessPolicies.getValue(source)
    val protocolLimit = protocolTtlMs?.coerceAtLeast(0L) ?: Long.MAX_VALUE
    val removeAt = minOf(policy.removeAfterMs, protocolLimit)
    val staleAt = minOf(policy.staleAfterMs, removeAt)
    val age = saturatingElapsedAge(now = now, seenAt = seenAt)

    return when {
        age >= removeAt -> FindingFreshness.EXPIRED
        sourceHealth == SourceHealthState.PAUSED -> FindingFreshness.PAUSED_CACHED
        sourceHealth != SourceHealthState.LIVE -> FindingFreshness.STALE
        age >= staleAt -> FindingFreshness.STALE
        else -> FindingFreshness.LIVE
    }
}

fun resumedHealth(hasRetainedRows: Boolean): SourceHealthState =
    if (hasRetainedRows) SourceHealthState.STALE else SourceHealthState.LOADING

fun agedSourceHealth(
    health: PrivacySourceHealth,
    nowElapsedMs: Long,
): PrivacySourceHealth {
    if (health.state != SourceHealthState.LIVE) return health
    val lastSuccess = health.lastSuccessElapsedMs
        ?: return health.copy(state = SourceHealthState.LOADING)
    val staleAfterMs = PrivacyFreshnessPolicies.getValue(health.source).staleAfterMs
    val age = saturatingElapsedAge(now = nowElapsedMs, seenAt = lastSuccess)
    return if (age >= staleAfterMs) health.copy(state = SourceHealthState.STALE) else health
}

private fun saturatingElapsedAge(now: Long, seenAt: Long): Long {
    if (now <= seenAt) return 0L
    val difference = now - seenAt
    return if (difference < 0L) Long.MAX_VALUE else difference
}
