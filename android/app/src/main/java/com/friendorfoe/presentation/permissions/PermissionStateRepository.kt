package com.friendorfoe.presentation.permissions

import android.Manifest
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.friendorfoe.data.preferences.AppPreferences
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first

interface PlatformPermissionEvidence {
    val sdkInt: Int
    fun isGranted(permission: String): Boolean
    fun notificationsEnabled(): Boolean
    fun channelEnabled(channelId: String): Boolean
}

interface PermissionStateSource {
    val states: StateFlow<Map<AppFeature, PermissionUiState>>
}

@Singleton
class AndroidPlatformPermissionEvidence @Inject constructor(
    @ApplicationContext private val context: Context,
) : PlatformPermissionEvidence {
    override val sdkInt: Int
        get() = Build.VERSION.SDK_INT

    override fun isGranted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    override fun notificationsEnabled(): Boolean =
        NotificationManagerCompat.from(context).areNotificationsEnabled()

    override fun channelEnabled(channelId: String): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return true
        val manager = context.getSystemService(NotificationManager::class.java)
        val channel = manager?.getNotificationChannel(channelId) ?: return true
        return channel.importance != NotificationManager.IMPORTANCE_NONE
    }
}

@Module
@InstallIn(SingletonComponent::class)
abstract class PermissionEvidenceModule {
    @Binds
    abstract fun bindPlatformPermissionEvidence(
        implementation: AndroidPlatformPermissionEvidence,
    ): PlatformPermissionEvidence

    @Binds
    abstract fun bindPermissionStateSource(
        implementation: PermissionStateRepository,
    ): PermissionStateSource
}

@Singleton
class PermissionStateRepository @Inject constructor(
    private val platform: PlatformPermissionEvidence,
    private val preferences: AppPreferences,
) : PermissionStateSource {
    private val refreshGeneration = AtomicLong(0L)
    private val _states = MutableStateFlow<Map<AppFeature, PermissionUiState>>(
        AppFeature.entries.associateWith { PermissionUiState.Loading }
    )
    override val states: StateFlow<Map<AppFeature, PermissionUiState>> = _states.asStateFlow()

    suspend fun refresh(
        rationaleByPermission: Map<String, Boolean>,
    ): Map<AppFeature, PermissionUiState> {
        val generation = refreshGeneration.incrementAndGet()
        val platformSnapshot = snapshotPlatformEvidence(rationaleByPermission)
        val requestedBefore = preferences.requestedPermissions.first()
        val next = AppFeature.entries.associateWith { feature ->
            evaluate(feature, platformSnapshot, requestedBefore)
        }
        if (refreshGeneration.get() == generation) {
            _states.value = next
        }
        return next
    }

    fun stateFor(feature: AppFeature): PermissionUiState =
        states.value[feature] ?: PermissionUiState.Loading

    fun missingPermissionsFor(feature: AppFeature): Set<String> =
        requiredPermissions(feature, platform.sdkInt).filterTo(linkedSetOf()) {
            !platform.isGranted(it)
        }

    private fun evaluate(
        feature: AppFeature,
        platformSnapshot: PlatformPermissionSnapshot,
        requestedBefore: Set<String>,
    ): PermissionUiState {
        val required = requiredPermissions(feature, platformSnapshot.sdkInt)
        val runtimeState = if (feature == AppFeature.AR_MAP_LOCATION) {
            evaluateLocationPermission(
                fineGranted = platformSnapshot.isGranted(Manifest.permission.ACCESS_FINE_LOCATION),
                coarseGranted = platformSnapshot.isGranted(Manifest.permission.ACCESS_COARSE_LOCATION),
                requestedBefore = required.any(requestedBefore::contains),
                shouldShowRationale = required.any(platformSnapshot::shouldShowRationale),
            )
        } else {
            evaluateFeaturePermission(
                required.map { permission ->
                    PermissionEvidence(
                        permission = permission,
                        granted = platformSnapshot.isGranted(permission),
                        requestedBefore = permission in requestedBefore,
                        shouldShowRationale = platformSnapshot.shouldShowRationale(permission),
                    )
                }
            )
        }

        val channelId = notificationChannelId(feature) ?: return runtimeState
        return evaluateNotificationPermission(
            runtimePermission = runtimeState,
            notificationsEnabled = platformSnapshot.notificationsEnabled,
            channelEnabled = platformSnapshot.channelEnabled(channelId),
        )
    }

    private fun snapshotPlatformEvidence(
        rationaleByPermission: Map<String, Boolean>,
    ): PlatformPermissionSnapshot {
        val sdkInt = platform.sdkInt
        val relevantPermissions = AppFeature.entries
            .flatMapTo(linkedSetOf()) { requiredPermissions(it, sdkInt) }
        return PlatformPermissionSnapshot(
            sdkInt = sdkInt,
            grantedPermissions = relevantPermissions.filterTo(linkedSetOf(), platform::isGranted),
            rationaleByPermission = rationaleByPermission.toMap(),
            notificationsEnabled = platform.notificationsEnabled(),
            enabledChannels = setOf(PRIVACY_ALERT_CHANNEL_ID, SKY_ALERT_CHANNEL_ID)
                .filterTo(linkedSetOf(), platform::channelEnabled),
        )
    }
}

private data class PlatformPermissionSnapshot(
    val sdkInt: Int,
    val grantedPermissions: Set<String>,
    val rationaleByPermission: Map<String, Boolean>,
    val notificationsEnabled: Boolean,
    val enabledChannels: Set<String>,
) {
    fun isGranted(permission: String): Boolean = permission in grantedPermissions
    fun shouldShowRationale(permission: String): Boolean =
        rationaleByPermission[permission] == true

    fun channelEnabled(channelId: String): Boolean = channelId in enabledChannels
}
