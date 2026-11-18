package com.friendorfoe.data.repository

import android.content.Context
import com.google.android.play.core.appupdate.AppUpdateManagerFactory
import com.google.android.play.core.install.model.UpdateAvailability
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlin.coroutines.resume
import kotlinx.coroutines.suspendCancellableCoroutine

enum class PlayUpdateStatus { CURRENT, AVAILABLE }

interface AppUpdateRepository {
    suspend fun latest(): Result<PlayUpdateStatus>
}

class PlayAppUpdateRepository @Inject constructor(
    @ApplicationContext context: Context,
) : AppUpdateRepository {
    private val manager = AppUpdateManagerFactory.create(context)

    override suspend fun latest(): Result<PlayUpdateStatus> = suspendCancellableCoroutine { continuation ->
        manager.appUpdateInfo
            .addOnSuccessListener { info -> continuation.resume(playUpdateStatus(info.updateAvailability())) }
            .addOnFailureListener { failure -> continuation.resume(Result.failure(failure)) }
            .addOnCanceledListener {
                continuation.resume(Result.failure(IllegalStateException("Google Play canceled the check")))
            }
    }
}

internal fun playUpdateStatus(availability: Int): Result<PlayUpdateStatus> = when (availability) {
    UpdateAvailability.UPDATE_AVAILABLE, UpdateAvailability.DEVELOPER_TRIGGERED_UPDATE_IN_PROGRESS ->
        Result.success(PlayUpdateStatus.AVAILABLE)
    UpdateAvailability.UPDATE_NOT_AVAILABLE -> Result.success(PlayUpdateStatus.CURRENT)
    else -> Result.failure(IllegalStateException("Google Play could not determine availability"))
}
