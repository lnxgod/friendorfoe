package com.friendorfoe.data.repository

import com.friendorfoe.domain.model.SkyObject

enum class SkySourceKind(val label: String) {
    ADS_B("ADS-B"),
    BLE_REMOTE_ID("Remote ID · Bluetooth"),
    WIFI_REMOTE_ID("Remote ID · Wi-Fi"),
    PHONE_DERIVED("Phone"),
}

data class SkySourceSnapshot(
    val source: SkySourceKind,
    val enabled: Boolean,
    val resolved: Boolean,
    val lastSuccessElapsedMs: Long?,
    val failure: String?,
    val rows: List<SkyObject>,
)

data class SkySourceFailure(
    val source: SkySourceKind,
    val message: String,
    val lastSuccessElapsedMs: Long?,
    val cachedRowCount: Int,
)

data class SkySourceResolution(
    val resolved: Boolean,
    val rows: List<SkyObject>,
    val failures: List<SkySourceFailure>,
    val latestSuccessElapsedMs: Long?,
) {
    val failure: String?
        get() = failures
            .joinToString(" · ") { "${it.source.label}: ${it.message}" }
            .takeIf { it.isNotEmpty() }
}

fun reduceSkySources(
    snapshots: Map<SkySourceKind, SkySourceSnapshot>,
    expectedEnabledSources: Set<SkySourceKind>,
): SkySourceResolution {
    val registeredExpectedSources = expectedEnabledSources
        .sortedBy { it.ordinal }
        .mapNotNull { expected ->
            snapshots[expected]?.takeIf { snapshot ->
                snapshot.enabled && snapshot.source == expected
            }
        }
    val allExpectedSourcesRegistered =
        registeredExpectedSources.size == expectedEnabledSources.size

    return SkySourceResolution(
        resolved = allExpectedSourcesRegistered && registeredExpectedSources.all { it.resolved },
        rows = registeredExpectedSources.flatMap { it.rows },
        failures = registeredExpectedSources.mapNotNull { snapshot ->
            snapshot.failure?.let { message ->
                SkySourceFailure(
                    source = snapshot.source,
                    message = message,
                    lastSuccessElapsedMs = snapshot.lastSuccessElapsedMs,
                    cachedRowCount = snapshot.rows.size,
                )
            }
        },
        latestSuccessElapsedMs = registeredExpectedSources
            .mapNotNull { it.lastSuccessElapsedMs }
            .maxOrNull(),
    )
}
