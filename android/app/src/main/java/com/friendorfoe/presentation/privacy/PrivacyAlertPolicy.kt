package com.friendorfoe.presentation.privacy

import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class PrivacyAlertPolicy internal constructor(
    private val cooldownMs: Long,
) {
    @Inject
    constructor() : this(FIVE_MINUTES_MS)

    private data class Published(val severity: FindingSeverity, val atElapsedMs: Long)

    private var activeKeys = emptySet<PrivacyFindingKey>()
    private val published = mutableMapOf<PrivacyFindingKey, Published>()

    @Synchronized
    fun newAlerts(
        alertEligible: List<PrivacyFinding>,
        nowElapsedMs: Long,
    ): List<PrivacyFinding> {
        val currentKeys = alertEligible.mapNotNull(PrivacyFinding::routableKey).toSet()
        val alerts = alertEligible.filter { finding ->
            val key = finding.routableKey ?: return@filter false
            val prior = published[key]
            val edge = key !in activeKeys
            val unpublished = prior == null
            val severityRose = prior != null && finding.severity.rank > prior.severity.rank
            val cooldownElapsed = prior == null ||
                elapsedSince(prior.atElapsedMs, nowElapsedMs) >= cooldownMs
            (edge || unpublished || severityRose) && cooldownElapsed
        }
        activeKeys = currentKeys
        return alerts
    }

    @Synchronized
    fun markPublished(finding: PrivacyFinding, nowElapsedMs: Long) {
        val key = finding.routableKey ?: return
        published[key] = Published(finding.severity, nowElapsedMs)
        activeKeys = activeKeys + key
    }

    private fun elapsedSince(then: Long, now: Long): Long =
        if (now >= then) now - then else Long.MAX_VALUE

    private companion object {
        const val FIVE_MINUTES_MS = 5 * 60 * 1_000L
    }
}
