package com.friendorfoe.presentation.privacy

import com.friendorfoe.presentation.permissions.AppFeature

enum class BackendOnlyExit { NOT_REQUESTED, NEEDS_CONFIRMATION, CONFIRMED }

/** A permission recovery requested by the user and kept across screen recreation. */
data class PrivacyPermissionAction(
    val source: PrivacySourceKind,
    val requestLaunched: Boolean,
    val settingsLaunchFailed: Boolean = false,
    val backendOnlyExit: BackendOnlyExit = BackendOnlyExit.NOT_REQUESTED,
)

internal fun permissionFeatureForPrivacySource(source: PrivacySourceKind): AppFeature? = when (source) {
    PrivacySourceKind.PHONE_BLE -> AppFeature.PHONE_PRIVACY_SCAN
    PrivacySourceKind.WIFI_ANALYSIS -> AppFeature.WIFI_PRIVACY_SCAN

    PrivacySourceKind.PHONE_ULTRASONIC -> AppFeature.ULTRASONIC
    else -> null
}
