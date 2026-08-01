package com.friendorfoe.presentation.ar

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SheetValue
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp

class ArCaptureInteractions(
    private val reviewViewModel: CaptureReviewViewModel,
    private val onObjectPeekRequested: (String) -> Unit,
    private val onObjectPeekInspectRequested: (ObjectPeekState) -> Unit = {},
    private val onObjectPeekDismissRequested: () -> Unit = {},
    private val requestPhotoDraft: (String, (CaptureDraft?) -> Unit) -> Unit = { _, callback ->
        callback(null)
    },
    private val onFullDetailsRequested: (String) -> Unit = {},
) {
    fun onLabelTapped(objectId: String) {
        onObjectPeekRequested(objectId)
    }

    fun captureWith(requestDraft: ((CaptureDraft?) -> Unit) -> Unit) {
        requestDraft(::reviewCapturedDraft)
    }

    fun reviewCapturedDraft(draft: CaptureDraft?) {
        draft?.let(reviewViewModel::inspect)
    }

    fun inspectObjectPeek(state: ObjectPeekState) {
        onObjectPeekInspectRequested(state)
    }

    fun captureObjectPeek(state: ObjectPeekState) {
        onObjectPeekDismissRequested()
        captureWith { callback -> requestPhotoDraft(state.title, callback) }
    }

    fun openFullDetails(objectId: String) {
        onObjectPeekDismissRequested()
        onFullDetailsRequested(objectId)
    }
}

class CaptureSaveInteractions(
    private val reviewViewModel: CaptureReviewViewModel,
    private val apiLevel: () -> Int,
    private val hasLegacyWritePermission: () -> Boolean,
    private val requestLegacyWritePermission: () -> Unit,
) {
    fun save() {
        val sdk = apiLevel()
        val permissionGranted = sdk > 28 || hasLegacyWritePermission()
        when (captureSavePermissionDecision(sdk, permissionGranted)) {
            CaptureSavePermissionDecision.SaveNow -> reviewViewModel.save()
            CaptureSavePermissionDecision.RequestLegacyWrite -> requestLegacyWritePermission()
        }
    }

    fun onLegacyWritePermissionResult(granted: Boolean) {
        if (granted) {
            reviewViewModel.save()
        } else {
            reviewViewModel.savePermissionDenied()
        }
    }
}

internal fun captureReviewSheetTransitionAllowed(
    targetIsHidden: Boolean,
    saving: Boolean,
): Boolean = !targetIsHidden || !saving

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CaptureReviewModal(
    state: CaptureReviewState,
    onSave: () -> Unit,
    onShare: () -> Unit,
    onDiscard: () -> Unit,
    onRetrySave: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val savingState = rememberUpdatedState(state is CaptureReviewState.Saving)
    val sheetState = androidx.compose.material3.rememberModalBottomSheetState(
        skipPartiallyExpanded = false,
        confirmValueChange = { target ->
            captureReviewSheetTransitionAllowed(
                targetIsHidden = target == SheetValue.Hidden,
                saving = savingState.value,
            )
        },
    )
    ModalBottomSheet(
        onDismissRequest = {
            if (!savingState.value) onDiscard()
        },
        sheetState = sheetState,
        modifier = modifier,
    ) {
        CaptureReviewScreen(
            state = state,
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = onRetrySave,
        )
    }
}

@Composable
fun CaptureReviewScreen(
    state: CaptureReviewState,
    onSave: () -> Unit,
    onShare: () -> Unit,
    onDiscard: () -> Unit,
    onRetrySave: () -> Unit,
    modifier: Modifier = Modifier,
) {
    when (state) {
        CaptureReviewState.Empty -> Unit
        is CaptureReviewState.Saved -> {
            Column(
                modifier = modifier
                    .fillMaxWidth()
                    .padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text("Saved to Photos", style = MaterialTheme.typography.titleMedium)
                TextButton(onClick = onDiscard) { Text("Done") }
            }
        }
        is CaptureReviewState.SavePermissionDenied -> ReviewContent(
            draft = state.draft,
            busy = false,
            error = "Photos access was not granted. Grant access to save this capture.",
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = onSave,
            modifier = modifier,
        )
        is CaptureReviewState.Reviewing -> ReviewContent(
            draft = state.draft,
            busy = false,
            error = null,
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = null,
            modifier = modifier,
        )
        is CaptureReviewState.Saving -> ReviewContent(
            draft = state.draft,
            busy = true,
            error = null,
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = null,
            modifier = modifier,
        )
        is CaptureReviewState.SaveFailed -> ReviewContent(
            draft = state.draft,
            busy = false,
            error = state.message,
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = onRetrySave,
            modifier = modifier,
        )
        is CaptureReviewState.ShareFailed -> ReviewContent(
            draft = state.draft,
            busy = false,
            error = state.message,
            onSave = onSave,
            onShare = onShare,
            onDiscard = onDiscard,
            onRetrySave = null,
            modifier = modifier,
        )
    }
}

@Composable
private fun ReviewContent(
    draft: CaptureDraft,
    busy: Boolean,
    error: String?,
    onSave: () -> Unit,
    onShare: () -> Unit,
    onDiscard: () -> Unit,
    onRetrySave: (() -> Unit)?,
    modifier: Modifier,
) {
    val preview = remember(draft) {
        BitmapFactory.decodeByteArray(draft.payload.bytes, 0, draft.payload.bytes.size)
    }
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(20.dp),
    ) {
        Text("Review capture", style = MaterialTheme.typography.titleLarge)
        Text(
            "Nothing is added to Photos until you tap Save.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 4.dp, bottom = 12.dp),
        )
        if (preview != null) {
            Image(
                bitmap = preview.asImageBitmap(),
                contentDescription = "Captured photo preview",
                contentScale = ContentScale.Fit,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 180.dp, max = 360.dp)
                    .background(Color.Black, RoundedCornerShape(12.dp)),
            )
        }
        error?.let {
            Text(
                text = it,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(top = 12.dp),
            )
        }
        Spacer(Modifier.height(16.dp))
        if (onRetrySave != null) {
            Button(onClick = onRetrySave, modifier = Modifier.fillMaxWidth()) {
                Text("Retry save")
            }
        } else {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Button(onClick = onSave, enabled = !busy, modifier = Modifier.weight(1f)) {
                    Text(if (busy) "Saving…" else "Save")
                }
                OutlinedButton(onClick = onShare, enabled = !busy, modifier = Modifier.weight(1f)) {
                    Text("Share")
                }
            }
        }
        if (!busy) {
            TextButton(onClick = onDiscard, modifier = Modifier.align(Alignment.CenterHorizontally)) {
                Text("Discard")
            }
        }
    }
}

@Composable
fun CaptureShutterButton(
    captureInProgress: Boolean,
    onCapture: () -> Unit,
    modifier: Modifier = Modifier,
) {
    IconButton(
        onClick = onCapture,
        enabled = !captureInProgress,
        modifier = modifier
            .background(
                if (captureInProgress) Color.Gray else Color.White,
                androidx.compose.foundation.shape.CircleShape,
            ),
    ) {
        Icon(
            imageVector = Icons.Default.CameraAlt,
            contentDescription = "Capture",
            tint = Color.Black,
        )
    }
}
