package com.friendorfoe.detection

import java.util.Locale

private val MAC_IDENTITY_PATTERN = Regex("^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")

internal fun canonicalPrivacyIdentity(value: String): String? {
    val trimmed = value.trim()
    if (trimmed.isEmpty()) return null

    val withoutPrefix = if (trimmed.startsWith("mac:", ignoreCase = true)) {
        trimmed.substring(4)
    } else {
        trimmed
    }
    val normalizedMac = withoutPrefix
        .replace('-', ':')
        .lowercase(Locale.ROOT)
    if (MAC_IDENTITY_PATTERN.matches(normalizedMac)) {
        return "mac:$normalizedMac"
    }

    return trimmed.lowercase(Locale.ROOT)
}

internal fun canonicalPrivacyIdentities(values: Iterable<String>): Set<String> =
    values.mapNotNull(::canonicalPrivacyIdentity).toSet()

internal fun GlassesDetection.canonicalPrivacyIdentityAliases(): Set<String> =
    canonicalPrivacyIdentities(seenMacs + mac + fingerprintKey)

internal fun privacyIdentityIsIgnored(
    identityAliases: Iterable<String>,
    ignoredIdentities: Iterable<String>,
): Boolean {
    val canonicalIgnored = canonicalPrivacyIdentities(ignoredIdentities)
    if (canonicalIgnored.isEmpty()) return false
    return canonicalPrivacyIdentities(identityAliases).any { it in canonicalIgnored }
}
