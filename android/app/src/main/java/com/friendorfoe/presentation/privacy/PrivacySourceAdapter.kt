package com.friendorfoe.presentation.privacy

import kotlinx.coroutines.flow.StateFlow

interface PrivacySourceAdapter {
    val adapterId: String
    val representedSources: Set<PrivacySourceKind>
    val snapshots: StateFlow<List<PrivacySourceSnapshot>>

    suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult =
        PrivacyRecoveryResult.Unsupported(source, "This source has no manual recovery action")
}

sealed interface PrivacyRecoveryResult {
    val source: PrivacySourceKind

    data class Recovered(override val source: PrivacySourceKind) : PrivacyRecoveryResult

    data class Unsupported(
        override val source: PrivacySourceKind,
        val reason: String,
    ) : PrivacyRecoveryResult

    data class Failed(
        override val source: PrivacySourceKind,
        val message: String,
    ) : PrivacyRecoveryResult

    data class SourceUnavailable(override val source: PrivacySourceKind) : PrivacyRecoveryResult
}

enum class PrivacyPreferenceResult {
    Updated,
    NotFound,
    NotPersistable,
    MalformedKey,
}
