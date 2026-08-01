package com.friendorfoe.presentation.ar

import android.graphics.Bitmap
import android.provider.MediaStore
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.click
import androidx.compose.ui.test.swipeDown
import androidx.compose.ui.unit.dp
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.detection.VisualDetection
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.sensor.DeviceOrientation
import com.friendorfoe.sensor.ScreenPosition
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class ObjectPeekTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun exactLabelTapOpensPeekWithoutWriting() {
        val writer = RecordingPhotoWriter()
        val review = CaptureReviewViewModel(writer, RecordingShareFactory())
        compose.setContent {
            var peek by remember { mutableStateOf<ObjectPeekState?>(null) }
            val interactions = remember {
                ArCaptureInteractions(
                    reviewViewModel = review,
                    onObjectPeekRequested = { peek = peekState() },
                )
            }
            FriendOrFoeTheme {
                Box {
                    ArOverlay(
                        screenPositions = listOf(screenPosition()),
                        unmatchedVisuals = emptyList(),
                        classifiedUnknowns = emptyList(),
                        darkTargetScores = emptyList(),
                        lockedObjectId = null,
                        lockedScreenPosition = null,
                        orientation = DeviceOrientation(),
                        onLabelTapped = interactions::onLabelTapped,
                        onLabelLongPressed = {},
                        onVisualTapped = {},
                        onEmptySpaceTapped = {},
                        onReticleTapped = {},
                        modifier = Modifier.size(400.dp).testTag("ar_overlay"),
                    )
                    peek?.let {
                        ObjectPeek(it, {}, {}, {}, { peek = null })
                    }
                }
            }
        }

        compose.onNodeWithTag("ar_overlay").performTouchInput { click(center) }

        compose.onNodeWithText("TEST123").assertIsDisplayed()
        assertEquals(0, writer.writes.size)
    }

    @Test
    fun nearestLabelFallbackAlsoOpensPeekWithoutWriting() {
        val writer = RecordingPhotoWriter()
        val review = CaptureReviewViewModel(writer, RecordingShareFactory())
        compose.setContent {
            var peek by remember { mutableStateOf<ObjectPeekState?>(null) }
            val interactions = remember {
                ArCaptureInteractions(
                    reviewViewModel = review,
                    onObjectPeekRequested = { peek = peekState() },
                )
            }
            FriendOrFoeTheme {
                Box {
                    ArOverlay(
                        screenPositions = listOf(screenPosition()),
                        unmatchedVisuals = emptyList(),
                        classifiedUnknowns = emptyList(),
                        darkTargetScores = emptyList(),
                        lockedObjectId = null,
                        lockedScreenPosition = null,
                        orientation = DeviceOrientation(),
                        onLabelTapped = interactions::onLabelTapped,
                        onLabelLongPressed = {},
                        onVisualTapped = {},
                        onEmptySpaceTapped = {},
                        onReticleTapped = {},
                        modifier = Modifier.size(400.dp).testTag("ar_overlay"),
                    )
                    peek?.let { ObjectPeek(it, {}, {}, {}, { peek = null }) }
                }
            }
        }

        compose.onNodeWithTag("ar_overlay").performTouchInput {
            click(Offset(center.x, center.y + 100f))
        }

        compose.onNodeWithText("TEST123").assertIsDisplayed()
        assertEquals(0, writer.writes.size)
    }

    @Test
    fun inspectIsReadOnly() {
        val before = galleryRowCount()
        val review = CaptureReviewViewModel(FailOnWritePhotoWriter, RecordingShareFactory())
        var inspectCalls = 0
        var inspectedEvidence: String? = null
        val interactions = ArCaptureInteractions(
            reviewViewModel = review,
            onObjectPeekRequested = {},
            onObjectPeekInspectRequested = { peek ->
                inspectCalls += 1
                inspectedEvidence = peek.evidence
            },
        )
        compose.setContent {
            FriendOrFoeTheme {
                ObjectPeek(
                    peekState(),
                    { interactions.inspectObjectPeek(peekState()) },
                    {},
                    {},
                    {},
                )
            }
        }

        compose.onNodeWithText("Inspect").performClick()
        compose.waitForIdle()

        assertEquals(1, inspectCalls)
        assertEquals("ADS-B radio match", inspectedEvidence)
        assertEquals(before, galleryRowCount())
    }

    @Test
    fun zoomIsInspectOnly() {
        val before = galleryRowCount()
        val bitmap = Bitmap.createBitmap(40, 40, Bitmap.Config.ARGB_8888)
        compose.setContent {
            FriendOrFoeTheme {
                ZoomViewSheet(
                    detection = visualDetection(),
                    classified = null,
                    getFrame = { bitmap },
                    evidence = "ADS-B radio match",
                    onDismiss = {},
                )
            }
        }

        compose.onNodeWithText("Inspect only — no photo has been saved.").assertIsDisplayed()
        compose.onNodeWithText("ADS-B radio match").assertIsDisplayed()
        compose.onNodeWithText("No radio match is currently available").assertDoesNotExist()
        compose.waitForIdle()
        assertEquals(before, galleryRowCount())
    }

    @Test
    fun fullDetailsIsReadOnly() {
        val before = galleryRowCount()
        val review = CaptureReviewViewModel(FailOnWritePhotoWriter, RecordingShareFactory())
        var detailsCalls = 0
        val interactions = ArCaptureInteractions(
            reviewViewModel = review,
            onObjectPeekRequested = {},
            onFullDetailsRequested = { detailsCalls += 1 },
        )
        compose.setContent {
            FriendOrFoeTheme {
                ObjectPeek(
                    peekState(),
                    {},
                    {},
                    { interactions.openFullDetails(peekState().objectId) },
                    {},
                )
            }
        }

        compose.onNodeWithText("Full details").performClick()
        compose.waitForIdle()

        assertEquals(1, detailsCalls)
        assertEquals(before, galleryRowCount())
    }

    @Test
    fun captureOnlyCreatesReviewDraft() {
        val writer = RecordingPhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory())
        val draft = captureDraft()
        val requestedLabels = mutableListOf<String>()
        var dismissCalls = 0
        val interactions = ArCaptureInteractions(
            reviewViewModel = viewModel,
            onObjectPeekRequested = {},
            onObjectPeekDismissRequested = { dismissCalls += 1 },
            requestPhotoDraft = { label, callback ->
                requestedLabels += label
                callback(draft)
            },
        )
        compose.setContent {
            FriendOrFoeTheme {
                ObjectPeek(
                    peekState(),
                    {},
                    { interactions.captureObjectPeek(peekState()) },
                    {},
                    {},
                )
            }
        }

        compose.onNodeWithText("Capture").performClick()

        assertEquals(CaptureReviewState.Reviewing(draft), viewModel.state.value)
        assertEquals(0, writer.writes.size)
        assertEquals(listOf("TEST123"), requestedLabels)
        assertEquals(1, dismissCalls)
    }

    @Test
    fun shareUsesCacheFactoryAndNeverWriter() {
        val writer = RecordingPhotoWriter()
        val shares = RecordingShareFactory()
        val viewModel = CaptureReviewViewModel(writer, shares).apply { inspect(captureDraft()) }
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
            }
        }

        compose.onNodeWithText("Share").performClick()
        compose.waitUntil { shares.requests.size == 1 }

        assertEquals(0, writer.writes.size)
    }

    @Test
    fun discardNeverWrites() {
        val writer = RecordingPhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory()).apply { inspect(captureDraft()) }
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
            }
        }

        compose.onNodeWithText("Discard").performClick()

        assertEquals(CaptureReviewState.Empty, viewModel.state.value)
        assertEquals(0, writer.writes.size)
    }

    @Test
    fun explicitSaveWritesExactlyOnce() {
        val writer = RecordingPhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory()).apply { inspect(captureDraft()) }
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
            }
        }

        compose.onNodeWithText("Save").performClick()
        compose.waitUntil { writer.writes.size == 1 }

        assertEquals(1, writer.writes.size)
        compose.onNodeWithText("Saved to Photos").assertIsDisplayed()
    }

    @Test
    fun mainShutterRequiresReviewThenSave() {
        val writer = RecordingPhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory())
        val interactions = ArCaptureInteractions(viewModel, onObjectPeekRequested = {})
        val draft = captureDraft()
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                if (state is CaptureReviewState.Empty) {
                    CaptureShutterButton(
                        captureInProgress = false,
                        onCapture = {
                            interactions.captureWith { callback -> callback(draft) }
                        },
                    )
                } else {
                    CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
                }
            }
        }

        compose.onNodeWithContentDescription("Capture").performClick()
        compose.onNodeWithText("Review capture").assertIsDisplayed()
        assertEquals(0, writer.writes.size)
        compose.onNodeWithText("Save").performClick()
        compose.waitUntil { writer.writes.size == 1 }

        assertEquals(1, writer.writes.size)
    }

    @Test
    fun snapPhotoSheetRequiresReviewThenSave() {
        val writer = RecordingPhotoWriter()
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory())
        val interactions = ArCaptureInteractions(viewModel, onObjectPeekRequested = {})
        val draft = captureDraft()
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                if (state is CaptureReviewState.Empty) {
                    SnapPhotoSheet(
                        target = SnapTarget("abc123", "TEST123", "Aircraft", 500.0),
                        getFrame = { null },
                        currentZoomRatio = 1f,
                        minZoomRatio = 1f,
                        maxZoomRatio = 1f,
                        onZoomChange = {},
                        onCapture = { callback -> callback(draft) },
                        onReview = interactions::reviewCapturedDraft,
                        onViewDetails = {},
                        onDismiss = {},
                    )
                } else {
                    CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
                }
            }
        }

        compose.onNodeWithContentDescription("Capture").performClick()
        compose.onNodeWithText("Review capture").assertIsDisplayed()
        assertEquals(0, writer.writes.size)
        compose.onNodeWithText("Save").performClick()
        compose.waitUntil { writer.writes.size == 1 }

        assertEquals(1, writer.writes.size)
    }

    @Test
    fun saveFailureOffersRetryAndDiscard() {
        val writer = RecordingPhotoWriter(
            results = ArrayDeque(
                listOf(
                    Result.failure(IllegalStateException("disk full")),
                    Result.success(SavedPhoto("content://friendorfoe/retry")),
                ),
            ),
        )
        val viewModel = CaptureReviewViewModel(writer, RecordingShareFactory()).apply { inspect(captureDraft()) }
        compose.setContent {
            val state by viewModel.state.collectAsState()
            FriendOrFoeTheme {
                CaptureReviewScreen(state, { viewModel.save() }, { viewModel.share() }, { viewModel.discard() }, { viewModel.retrySave() })
            }
        }

        compose.onNodeWithText("Save").performClick()
        compose.onNodeWithText("Retry save").assertIsDisplayed().performClick()
        compose.waitUntil { writer.writes.size == 2 }

        assertEquals(2, writer.writes.size)
        compose.onNodeWithText("Saved to Photos").assertIsDisplayed()
    }

    @Test
    fun savingReviewCannotBeDiscarded() {
        var discardCalls = 0
        compose.setContent {
            FriendOrFoeTheme {
                CaptureReviewScreen(
                    state = CaptureReviewState.Saving(captureDraft()),
                    onSave = {},
                    onShare = {},
                    onDiscard = { discardCalls += 1 },
                    onRetrySave = {},
                )
            }
        }

        compose.onNodeWithText("Saving…").assertIsDisplayed()
        compose.onNodeWithText("Discard").assertDoesNotExist()
        assertEquals(0, discardCalls)
    }

    @Test
    fun savingReviewSheetRejectsSwipeToHide() {
        var discardCalls = 0
        compose.setContent {
            FriendOrFoeTheme {
                CaptureReviewModal(
                    state = CaptureReviewState.Saving(captureDraft()),
                    onSave = {},
                    onShare = {},
                    onDiscard = { discardCalls += 1 },
                    onRetrySave = {},
                    modifier = Modifier.testTag("capture_review_modal"),
                )
            }
        }

        compose.waitForIdle()
        compose.onNodeWithTag("capture_review_modal").performTouchInput {
            swipeDown(durationMillis = 500)
        }
        compose.waitForIdle()

        compose.onNodeWithText("Review capture").assertIsDisplayed()
        assertEquals(0, discardCalls)
    }

    @Test
    fun deniedLegacyPermissionKeepsDraftAndOffersHonestRetry() {
        val draft = captureDraft()
        var retryCalls = 0
        compose.setContent {
            FriendOrFoeTheme {
                CaptureReviewScreen(
                    state = CaptureReviewState.SavePermissionDenied(draft),
                    onSave = { retryCalls += 1 },
                    onShare = {},
                    onDiscard = {},
                    onRetrySave = {},
                )
            }
        }

        compose.onNodeWithText(
            "Photos access was not granted. Grant access to save this capture.",
        ).assertIsDisplayed()
        compose.onNodeWithText("Retry save").performClick()
        assertEquals(1, retryCalls)
    }
}

private object FailOnWritePhotoWriter : PhotoWriter {
    override suspend fun write(draft: CaptureDraft): Result<SavedPhoto> =
        error("A read-only Object Peek route attempted to write a photo")
}

private fun galleryRowCount(): Int {
    val resolver = InstrumentationRegistry.getInstrumentation().targetContext.contentResolver
    return resolver.query(
        MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
        arrayOf(MediaStore.Images.Media._ID),
        null,
        null,
        null,
    )?.use { it.count } ?: 0
}

private class RecordingPhotoWriter(
    private val results: ArrayDeque<Result<SavedPhoto>> = ArrayDeque(),
) : PhotoWriter {
    val writes = mutableListOf<CaptureDraft>()

    override suspend fun write(draft: CaptureDraft): Result<SavedPhoto> {
        writes += draft
        return results.removeFirstOrNull()
            ?: Result.success(SavedPhoto("content://friendorfoe/saved"))
    }
}

private class RecordingShareFactory : ShareImageFactory {
    val requests = mutableListOf<CaptureDraft>()

    override suspend fun create(draft: CaptureDraft): Result<ShareRequest> {
        requests += draft
        return Result.success(ShareRequest("content://friendorfoe/shared", draft.payload.mimeType))
    }
}

private fun peekState() = ObjectPeekState(
    objectId = "abc123",
    title = "TEST123",
    evidence = "ADS-B radio match",
    canCapture = true,
)

private fun captureDraft() = CaptureDraft(
    payload = CapturePayload(byteArrayOf(1, 2, 3), "image/jpeg", 640, 480),
    displayName = "friendorfoe_test.jpg",
    description = "Visual capture",
)

private fun visualDetection() = VisualDetection(
    trackingId = 7,
    centerX = 0.5f,
    centerY = 0.5f,
    width = 0.2f,
    height = 0.2f,
    labels = listOf("Aircraft"),
    timestampMs = 1L,
)

private fun screenPosition() = ScreenPosition(
    skyObject = Aircraft(
        id = "abc123",
        position = Position(37.0, -122.0, 1_000.0),
        source = DetectionSource.ADS_B,
        category = ObjectCategory.COMMERCIAL,
        firstSeen = Instant.ofEpochMilli(1L),
        lastUpdated = Instant.ofEpochMilli(2L),
        icaoHex = "abc123",
        callsign = "TEST123",
    ),
    screenX = 0.5f,
    screenY = 0.5f,
    isInView = true,
    distanceMeters = 500.0,
    groundDistanceMeters = 450.0,
)
