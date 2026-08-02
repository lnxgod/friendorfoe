package com.friendorfoe.presentation.permissions

import android.Manifest

enum class AppFeature {
    APP_LAUNCH,
    AR_CAMERA,
    IR_CAMERA,
    AR_MAP_LOCATION,
    LOCAL_RADIO_DISCOVERY,
    PHONE_PRIVACY_SCAN,
    PRIVACY_ALERTS,
    SKY_ALERTS,
    ULTRASONIC,
}

sealed interface PermissionUiState {
    data object Loading : PermissionUiState
    data object Granted : PermissionUiState
    data object Approximate : PermissionUiState
    data object Denied : PermissionUiState
    data object PermanentlyDenied : PermissionUiState
    data object NotificationsBlocked : PermissionUiState
    data object NotificationChannelBlocked : PermissionUiState
}

const val PRIVACY_ALERT_CHANNEL_ID = "privacy_alerts"
const val SKY_ALERT_CHANNEL_ID = "sky_alerts"

data class PermissionEvidence(
    val permission: String,
    val granted: Boolean,
    val requestedBefore: Boolean,
    val shouldShowRationale: Boolean,
)

fun requiredPermissions(feature: AppFeature, sdk: Int): Set<String> = when (feature) {
    AppFeature.APP_LAUNCH -> emptySet()
    AppFeature.AR_CAMERA,
    AppFeature.IR_CAMERA,
    -> setOf(Manifest.permission.CAMERA)

    AppFeature.AR_MAP_LOCATION -> setOf(
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION,
    )

    AppFeature.LOCAL_RADIO_DISCOVERY,
    AppFeature.PHONE_PRIVACY_SCAN,
    -> when {
        sdk >= 33 -> setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.NEARBY_WIFI_DEVICES,
            // WifiManager scan results still require fine location on API 33+.
            Manifest.permission.ACCESS_FINE_LOCATION,
        )

        sdk >= 31 -> setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )

        else -> setOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    AppFeature.PRIVACY_ALERTS,
    AppFeature.SKY_ALERTS,
    -> if (sdk >= 33) {
        setOf(Manifest.permission.POST_NOTIFICATIONS)
    } else {
        emptySet()
    }

    AppFeature.ULTRASONIC -> setOf(Manifest.permission.RECORD_AUDIO)
}

fun evaluatePermission(
    granted: Boolean,
    requestedBefore: Boolean,
    shouldShowRationale: Boolean,
): PermissionUiState = when {
    granted -> PermissionUiState.Granted
    requestedBefore && !shouldShowRationale -> PermissionUiState.PermanentlyDenied
    else -> PermissionUiState.Denied
}

fun evaluateFeaturePermission(
    evidence: List<PermissionEvidence>,
): PermissionUiState = when {
    evidence.isEmpty() || evidence.all(PermissionEvidence::granted) ->
        PermissionUiState.Granted

    evidence.any {
        !it.granted && it.requestedBefore && !it.shouldShowRationale
    } -> PermissionUiState.PermanentlyDenied

    else -> PermissionUiState.Denied
}

fun evaluateLocationPermission(
    fineGranted: Boolean,
    coarseGranted: Boolean,
    requestedBefore: Boolean,
    shouldShowRationale: Boolean,
): PermissionUiState = when {
    fineGranted -> PermissionUiState.Granted
    coarseGranted -> PermissionUiState.Approximate
    requestedBefore && !shouldShowRationale -> PermissionUiState.PermanentlyDenied
    else -> PermissionUiState.Denied
}

fun evaluateNotificationPermission(
    runtimePermission: PermissionUiState,
    notificationsEnabled: Boolean,
    channelEnabled: Boolean,
): PermissionUiState = when {
    runtimePermission != PermissionUiState.Granted -> runtimePermission
    !notificationsEnabled -> PermissionUiState.NotificationsBlocked
    !channelEnabled -> PermissionUiState.NotificationChannelBlocked
    else -> PermissionUiState.Granted
}

fun PermissionUiState.isUsable(): Boolean =
    this == PermissionUiState.Granted || this == PermissionUiState.Approximate

fun notificationChannelId(feature: AppFeature): String? = when (feature) {
    AppFeature.PRIVACY_ALERTS -> PRIVACY_ALERT_CHANNEL_ID
    AppFeature.SKY_ALERTS -> SKY_ALERT_CHANNEL_ID
    else -> null
}

fun permissionTitle(feature: AppFeature): String = when (feature) {
    AppFeature.APP_LAUNCH -> "Friend or Foe"
    AppFeature.AR_CAMERA -> "Camera for AR"
    AppFeature.IR_CAMERA -> "Camera for IR-like light"
    AppFeature.AR_MAP_LOCATION -> "Location for nearby results"
    AppFeature.LOCAL_RADIO_DISCOVERY -> "Nearby radio access"
    AppFeature.PHONE_PRIVACY_SCAN -> "Nearby-device access"
    AppFeature.PRIVACY_ALERTS,
    AppFeature.SKY_ALERTS,
    -> "Notifications"

    AppFeature.ULTRASONIC -> "Microphone for ultrasonic checks"
}

fun permissionExplanation(feature: AppFeature): String = when (feature) {
    AppFeature.APP_LAUNCH -> "No system permission is required to open Friend or Foe."
    AppFeature.AR_CAMERA ->
        "AR uses the camera to place detection information over the live view."

    AppFeature.IR_CAMERA ->
        "The IR-like light tool uses the camera to inspect light that may be hard to see."

    AppFeature.AR_MAP_LOCATION ->
        "Location centers nearby results. With approximate access, placement, distance, and bearing may be less accurate."

    AppFeature.LOCAL_RADIO_DISCOVERY ->
        "Bluetooth and Wi-Fi access lets this phone look for nearby broadcast signals. Android also requires fine location for Wi-Fi scan results because they can reveal nearby location."

    AppFeature.PHONE_PRIVACY_SCAN ->
        "The phone privacy scan uses Bluetooth and Wi-Fi observations. Android also requires fine location for Wi-Fi scan results because they can reveal nearby location."

    AppFeature.PRIVACY_ALERTS ->
        "Notifications let Friend or Foe alert you about eligible high-risk privacy findings."

    AppFeature.SKY_ALERTS ->
        "Notifications let Friend or Foe deliver the aircraft alert types you enable."

    AppFeature.ULTRASONIC ->
        "Ultrasonic checks sample a narrow high-frequency audio band with the microphone."
}

fun permissionRecovery(feature: AppFeature, state: PermissionUiState): String = when (state) {
    PermissionUiState.PermanentlyDenied ->
        "Android is no longer offering this permission here. Open app settings to allow ${permissionTitle(feature).lowercase()}."

    PermissionUiState.NotificationsBlocked ->
        "Notifications are disabled for Friend or Foe. Open app settings to restore delivery."

    PermissionUiState.NotificationChannelBlocked ->
        "This alert channel is disabled. Open its notification settings to restore delivery."

    else -> permissionExplanation(feature)
}
