package com.friendorfoe.presentation.permissions

import android.Manifest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FeaturePermissionsTest {

    @Test
    fun appLaunchNeverOwnsAPlatformPermission() {
        assertTrue(requiredPermissions(AppFeature.APP_LAUNCH, sdk = 35).isEmpty())
    }

    @Test
    fun api35RadioFeaturesIncludeEveryPermissionTheirCollectorsActuallyUse() {
        val expected = setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.NEARBY_WIFI_DEVICES,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )

        assertEquals(expected, requiredPermissions(AppFeature.LOCAL_RADIO_DISCOVERY, sdk = 35))
        assertEquals(expected, requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 35))
    }

    @Test
    fun api32RadioFeaturesUseBluetoothRuntimePermissionsAndFineLocation() {
        val expected = setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )

        assertEquals(expected, requiredPermissions(AppFeature.LOCAL_RADIO_DISCOVERY, sdk = 32))
        assertEquals(expected, requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 32))
    }

    @Test
    fun legacyRadioFeaturesUseFineLocation() {
        val expected = setOf(Manifest.permission.ACCESS_FINE_LOCATION)

        assertEquals(expected, requiredPermissions(AppFeature.LOCAL_RADIO_DISCOVERY, sdk = 30))
        assertEquals(expected, requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 30))
    }

    @Test
    fun cameraLocationNotificationsAndMicrophoneStayFeatureSpecific() {
        assertEquals(
            setOf(Manifest.permission.CAMERA),
            requiredPermissions(AppFeature.AR_CAMERA, sdk = 35),
        )
        assertEquals(
            setOf(Manifest.permission.CAMERA),
            requiredPermissions(AppFeature.IR_CAMERA, sdk = 35),
        )
        assertEquals(
            setOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION,
            ),
            requiredPermissions(AppFeature.AR_MAP_LOCATION, sdk = 35),
        )
        assertEquals(
            setOf(Manifest.permission.POST_NOTIFICATIONS),
            requiredPermissions(AppFeature.PRIVACY_ALERTS, sdk = 35),
        )
        assertEquals(
            setOf(Manifest.permission.POST_NOTIFICATIONS),
            requiredPermissions(AppFeature.SKY_ALERTS, sdk = 35),
        )
        assertEquals(
            setOf(Manifest.permission.RECORD_AUDIO),
            requiredPermissions(AppFeature.ULTRASONIC, sdk = 35),
        )
        assertTrue(requiredPermissions(AppFeature.PRIVACY_ALERTS, sdk = 32).isEmpty())
        assertTrue(requiredPermissions(AppFeature.SKY_ALERTS, sdk = 32).isEmpty())
    }

    @Test
    fun firstDenialIsNotMisreportedAsPermanent() {
        assertEquals(
            PermissionUiState.Denied,
            evaluatePermission(
                granted = false,
                requestedBefore = false,
                shouldShowRationale = false,
            ),
        )
    }

    @Test
    fun deniedPermissionBecomesPermanentOnlyAfterARecordedRequest() {
        assertEquals(
            PermissionUiState.PermanentlyDenied,
            evaluatePermission(
                granted = false,
                requestedBefore = true,
                shouldShowRationale = false,
            ),
        )
        assertEquals(
            PermissionUiState.Denied,
            evaluatePermission(
                granted = false,
                requestedBefore = true,
                shouldShowRationale = true,
            ),
        )
    }

    @Test
    fun onePermanentlyDeniedMemberMakesAMultiPermissionFeatureRecoverableInSettings() {
        assertEquals(
            PermissionUiState.PermanentlyDenied,
            evaluateFeaturePermission(
                listOf(
                    PermissionEvidence(
                        permission = Manifest.permission.BLUETOOTH_SCAN,
                        granted = false,
                        requestedBefore = true,
                        shouldShowRationale = false,
                    ),
                    PermissionEvidence(
                        permission = Manifest.permission.BLUETOOTH_CONNECT,
                        granted = false,
                        requestedBefore = false,
                        shouldShowRationale = false,
                    ),
                )
            ),
        )
    }

    @Test
    fun coarseOnlyLocationIsRepresentedAsApproximateRatherThanFine() {
        assertEquals(
            PermissionUiState.Approximate,
            evaluateLocationPermission(
                fineGranted = false,
                coarseGranted = true,
                requestedBefore = true,
                shouldShowRationale = false,
            ),
        )
    }

    @Test
    fun notificationDeliveryDistinguishesRuntimeGlobalAndChannelBlocks() {
        assertEquals(
            PermissionUiState.PermanentlyDenied,
            evaluateNotificationPermission(
                runtimePermission = PermissionUiState.PermanentlyDenied,
                notificationsEnabled = true,
                channelEnabled = true,
            ),
        )
        assertEquals(
            PermissionUiState.NotificationsBlocked,
            evaluateNotificationPermission(
                runtimePermission = PermissionUiState.Granted,
                notificationsEnabled = false,
                channelEnabled = true,
            ),
        )
        assertEquals(
            PermissionUiState.NotificationChannelBlocked,
            evaluateNotificationPermission(
                runtimePermission = PermissionUiState.Granted,
                notificationsEnabled = true,
                channelEnabled = false,
            ),
        )
        assertEquals(
            PermissionUiState.Granted,
            evaluateNotificationPermission(
                runtimePermission = PermissionUiState.Granted,
                notificationsEnabled = true,
                channelEnabled = true,
            ),
        )
    }
}
