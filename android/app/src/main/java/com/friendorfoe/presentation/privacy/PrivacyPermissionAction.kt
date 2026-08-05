package com.friendorfoe.presentation.privacy

import com.friendorfoe.presentation.permissions.AppFeature

/** A permission recovery requested by the user and kept across screen recreation. */
data class PrivacyPermissionAction(
    val source: PrivacySourceKind,
    val requestLaunched: Boolean,
    val settingsLaunchFailed: Boolean = false,
)

internal fun permissionFeatureForPrivacySource(source: PrivacySourceKind): AppFeature? = when (source) {
    PrivacySourceKind.PHONE_BLE,
    PrivacySourceKind.WIFI_ANALYSIS,
    -> AppFeature.PHONE_PRIVACY_SCAN

    PrivacySourceKind.PHONE_ULTRASONIC -> AppFeature.ULTRASONIC
    else -> null
}
