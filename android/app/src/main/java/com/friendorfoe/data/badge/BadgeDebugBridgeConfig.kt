package com.friendorfoe.data.badge

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

data class BadgeDebugBridgeConfig(
    val enabled: Boolean,
    val baseUrl: HttpUrl?,
)

fun badgeDebugBridgeConfig(
    isDebug: Boolean,
    configuredUrl: String,
): BadgeDebugBridgeConfig {
    val parsed = configuredUrl.toHttpUrlOrNull()
    return BadgeDebugBridgeConfig(
        enabled = isDebug && parsed != null,
        baseUrl = parsed.takeIf { isDebug },
    )
}
