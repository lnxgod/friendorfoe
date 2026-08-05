package com.friendorfoe.presentation.ar

data class CapturePayload(
    val bytes: ByteArray,
    val mimeType: String,
    val widthPx: Int,
    val heightPx: Int,
)

data class CaptureDraft(
    val payload: CapturePayload,
    val displayName: String,
    val description: String,
)

fun interface PhotoWriter {
    suspend fun write(draft: CaptureDraft): Result<SavedPhoto>
}

@JvmInline
value class SavedPhoto(val contentUri: String)

data class ShareRequest(
    val contentUri: String,
    val mimeType: String,
)

fun interface ShareImageFactory {
    suspend fun create(draft: CaptureDraft): Result<ShareRequest>
}

sealed interface CaptureReviewState {
    data object Empty : CaptureReviewState
    data class Reviewing(val draft: CaptureDraft) : CaptureReviewState
    data class Saving(val draft: CaptureDraft) : CaptureReviewState
    data class Saved(val photo: SavedPhoto) : CaptureReviewState
    data class SavePermissionDenied(
        val draft: CaptureDraft,
        val recovery: LegacyPhotoPermissionRecovery = LegacyPhotoPermissionRecovery.RetryRequest,
        val settingsLaunchFailed: Boolean = false,
    ) : CaptureReviewState
    data class SaveFailed(val draft: CaptureDraft, val message: String) : CaptureReviewState
    data class ShareFailed(val draft: CaptureDraft, val message: String) : CaptureReviewState
}

enum class LegacyPhotoPermissionRecovery {
    RetryRequest,
    OpenAppSettings,
}

sealed interface CaptureSavePermissionDecision {
    data object SaveNow : CaptureSavePermissionDecision
    data object RequestLegacyWrite : CaptureSavePermissionDecision
}

fun captureSavePermissionDecision(
    apiLevel: Int,
    legacyWriteGranted: Boolean,
): CaptureSavePermissionDecision =
    if (apiLevel <= 28 && !legacyWriteGranted) {
        CaptureSavePermissionDecision.RequestLegacyWrite
    } else {
        CaptureSavePermissionDecision.SaveNow
    }

sealed interface CaptureReviewEffect {
    data class LaunchShare(val request: ShareRequest) : CaptureReviewEffect
}
