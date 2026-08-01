package com.friendorfoe.presentation.ar

import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class CaptureReviewViewModelTest {
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun captureReviewSheetCannotHideWhileSaving() {
        assertFalse(captureReviewSheetTransitionAllowed(targetIsHidden = true, saving = true))
        assertTrue(captureReviewSheetTransitionAllowed(targetIsHidden = false, saving = true))
        assertTrue(captureReviewSheetTransitionAllowed(targetIsHidden = true, saving = false))
    }

    @Test
    fun legacySavePermissionPolicyOnlyRequestsAccessThroughExplicitSave() = runTest {
        assertEquals(
            CaptureSavePermissionDecision.RequestLegacyWrite,
            captureSavePermissionDecision(apiLevel = 28, legacyWriteGranted = false),
        )
        assertEquals(
            CaptureSavePermissionDecision.SaveNow,
            captureSavePermissionDecision(apiLevel = 28, legacyWriteGranted = true),
        )
        assertEquals(
            CaptureSavePermissionDecision.SaveNow,
            captureSavePermissionDecision(apiLevel = 29, legacyWriteGranted = false),
        )

        val writer = FakePhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        var permissionRequests = 0
        val interactions = CaptureSaveInteractions(
            reviewViewModel = viewModel,
            apiLevel = { 28 },
            hasLegacyWritePermission = { false },
            requestLegacyWritePermission = { permissionRequests += 1 },
        )
        val draft = captureDraft()
        viewModel.inspect(draft)

        interactions.save()

        assertEquals(1, permissionRequests)
        assertEquals(emptyList<CaptureDraft>(), writer.writes)
        assertEquals(CaptureReviewState.Reviewing(draft), viewModel.state.value)

        interactions.onLegacyWritePermissionResult(granted = false)
        assertEquals(
            CaptureReviewState.SavePermissionDenied(draft),
            viewModel.state.value,
        )

        interactions.save()
        assertEquals(2, permissionRequests)
        assertEquals(emptyList<CaptureDraft>(), writer.writes)

        interactions.onLegacyWritePermissionResult(granted = true)
        advanceUntilIdle()
        assertEquals(listOf(draft), writer.writes)
    }

    @Test
    fun modernSaveNeverRequestsLegacyPermission() = runTest {
        val writer = FakePhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        var permissionRequests = 0
        val interactions = CaptureSaveInteractions(
            reviewViewModel = viewModel,
            apiLevel = { 35 },
            hasLegacyWritePermission = { false },
            requestLegacyWritePermission = { permissionRequests += 1 },
        )
        val draft = captureDraft()
        viewModel.inspect(draft)

        interactions.save()
        advanceUntilIdle()

        assertEquals(0, permissionRequests)
        assertEquals(listOf(draft), writer.writes)
    }

    @Test
    fun onlyExplicitSaveWritesPhoto() = runTest {
        val writer = FakePhotoWriter()
        val shares = FakeShareImageFactory()
        val viewModel = CaptureReviewViewModel(writer, shares)
        val draft = captureDraft()

        viewModel.inspect(draft)
        viewModel.share()
        advanceUntilIdle()
        viewModel.discard()
        assertEquals(0, writer.writes.size)
        assertEquals(listOf(draft), shares.requests)

        viewModel.inspect(draft)
        viewModel.save()
        advanceUntilIdle()
        assertEquals(listOf(draft), writer.writes)
    }

    @Test
    fun failedSavePreservesDraftAndRetryWritesExactlyOnceMore() = runTest {
        val writer = FakePhotoWriter(
            results = ArrayDeque(
                listOf(
                    Result.failure(IllegalStateException("disk full")),
                    Result.success(SavedPhoto("content://friendorfoe/retry")),
                ),
            ),
        )
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        val draft = captureDraft()

        viewModel.inspect(draft)
        viewModel.save()!!.join()

        assertEquals(
            CaptureReviewState.SaveFailed(draft, "Could not save photo."),
            viewModel.state.value,
        )
        assertEquals(listOf(draft), writer.writes)

        viewModel.retrySave()!!.join()

        assertEquals(
            CaptureReviewState.Saved(SavedPhoto("content://friendorfoe/retry")),
            viewModel.state.value,
        )
        assertEquals(listOf(draft, draft), writer.writes)
    }

    @Test
    fun shareFailureNeverFallsBackToGalleryWriter() = runTest {
        val writer = FakePhotoWriter()
        val shares = FakeShareImageFactory(
            results = ArrayDeque(listOf(Result.failure(IllegalStateException("cache unavailable")))),
        )
        val viewModel = CaptureReviewViewModel(writer, shares)
        val draft = captureDraft()

        viewModel.inspect(draft)
        viewModel.share()!!.join()

        assertEquals(
            CaptureReviewState.ShareFailed(draft, "Could not prepare photo to share."),
            viewModel.state.value,
        )
        assertEquals(emptyList<CaptureDraft>(), writer.writes)
        assertEquals(listOf(draft), shares.requests)

        viewModel.save()!!.join()
        assertEquals(listOf(draft), writer.writes)
    }

    @Test
    fun discardedShareCannotEmitAStaleLaunchEffect() = runTest {
        val gate = CompletableDeferred<Unit>()
        val shares = FakeShareImageFactory(gate = gate)
        val viewModel = CaptureReviewViewModel(FakePhotoWriter(), shares)
        val effects = mutableListOf<CaptureReviewEffect>()
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            viewModel.effects.toList(effects)
        }

        viewModel.inspect(captureDraft())
        viewModel.share()
        runCurrent()
        viewModel.discard()
        gate.complete(Unit)
        advanceUntilIdle()

        assertEquals(emptyList<CaptureReviewEffect>(), effects)
    }

    @Test
    fun rapidSecondSaveWhileFirstIsActivePerformsOneWrite() = runTest {
        val gate = CompletableDeferred<Unit>()
        val writer = FakePhotoWriter(gate = gate)
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        val draft = captureDraft()

        viewModel.inspect(draft)
        val first = viewModel.save()!!
        runCurrent()
        val second = viewModel.save()
        runCurrent()

        assertEquals(listOf(draft), writer.writes)
        assertTrue(first.isActive)
        assertNull(second)

        gate.complete(Unit)
        advanceUntilIdle()
        assertEquals(listOf(draft), writer.writes)
    }

    @Test
    fun savingCannotBeDiscardedOrReplacedUntilTheWriteCompletes() = runTest {
        val gate = CompletableDeferred<Unit>()
        val writer = FakePhotoWriter(gate = gate)
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        val firstDraft = captureDraft()
        val secondDraft = captureDraft().copy(displayName = "friendorfoe_second.jpg")

        viewModel.inspect(firstDraft)
        val firstSave = viewModel.save()!!
        runCurrent()
        viewModel.discard()
        viewModel.inspect(secondDraft)
        val overlappingSave = viewModel.save()

        assertNull(overlappingSave)
        assertEquals(CaptureReviewState.Saving(firstDraft), viewModel.state.value)
        assertEquals(listOf(firstDraft), writer.writes)

        gate.complete(Unit)
        advanceUntilIdle()

        assertTrue(firstSave.isCompleted)
        assertEquals(
            CaptureReviewState.Saved(SavedPhoto("content://friendorfoe/saved")),
            viewModel.state.value,
        )
        assertEquals(listOf(firstDraft), writer.writes)

        viewModel.discard()
        viewModel.inspect(secondDraft)
        viewModel.save()!!.join()
        assertEquals(listOf(firstDraft, secondDraft), writer.writes)
    }

    @Test
    fun cancellationPropagatesWithoutRenderingSaveFailure() = runTest {
        val cancellation = CancellationException("screen closed")
        val writer = FakePhotoWriter(
            results = ArrayDeque(listOf(Result.failure(cancellation))),
        )
        val viewModel = CaptureReviewViewModel(writer, FakeShareImageFactory())
        val draft = captureDraft()

        viewModel.inspect(draft)
        var completion: Throwable? = null
        viewModel.save()!!.also { job ->
            job.invokeOnCompletion { completion = it }
            job.join()
        }

        assertTrue(completion is CancellationException)
        assertEquals(CaptureReviewState.Reviewing(draft), viewModel.state.value)
    }
}

private class FakePhotoWriter(
    private val results: ArrayDeque<Result<SavedPhoto>> = ArrayDeque(),
    private val gate: CompletableDeferred<Unit>? = null,
) : PhotoWriter {
    val writes = mutableListOf<CaptureDraft>()

    override suspend fun write(draft: CaptureDraft): Result<SavedPhoto> {
        writes += draft
        gate?.await()
        return results.removeFirstOrNull()
            ?: Result.success(SavedPhoto("content://friendorfoe/saved"))
    }
}

private class FakeShareImageFactory(
    private val results: ArrayDeque<Result<ShareRequest>> = ArrayDeque(),
    private val gate: CompletableDeferred<Unit>? = null,
) : ShareImageFactory {
    val requests = mutableListOf<CaptureDraft>()

    override suspend fun create(draft: CaptureDraft): Result<ShareRequest> {
        requests += draft
        gate?.await()
        return results.removeFirstOrNull()
            ?: Result.success(ShareRequest("content://friendorfoe/shared", "image/jpeg"))
    }
}

private fun captureDraft() = CaptureDraft(
    payload = CapturePayload(
        bytes = byteArrayOf(1, 2, 3),
        mimeType = "image/jpeg",
        widthPx = 640,
        heightPx = 480,
    ),
    displayName = "friendorfoe_test.jpg",
    description = "Visual capture",
)
