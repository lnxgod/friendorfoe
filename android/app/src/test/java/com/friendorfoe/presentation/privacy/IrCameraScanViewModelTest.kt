package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.IrCameraDetector
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class IrCameraScanViewModelTest {
    @Test
    fun startsInBindingStateWithoutInventingCameraOutput() {
        val viewModel = IrCameraScanViewModel()

        assertTrue(viewModel.uiState.value is IrCameraUiState.BindingCamera)
    }

    @Test
    fun acceptedFrameBecomesTheOnlyLiveEvidence() {
        val viewModel = IrCameraScanViewModel()
        val frame = frame()

        viewModel.onFrame(frame)

        val live = viewModel.uiState.value as IrCameraUiState.Live
        assertSame(frame, live.frame)
    }

    @Test
    fun bindFailureIsVisibleAndRetryStartsOneNewBindingGeneration() {
        val viewModel = IrCameraScanViewModel()
        viewModel.onBindFailure(IllegalStateException("Camera is in use"))

        assertEquals(
            IrCameraUiState.BindFailed("Camera is in use"),
            viewModel.uiState.value,
        )
        val beforeRetry = viewModel.bindingGeneration.value

        viewModel.retryBinding()

        assertEquals(beforeRetry + 1L, viewModel.bindingGeneration.value)
        assertTrue(viewModel.uiState.value is IrCameraUiState.BindingCamera)
    }

    @Test
    fun blankFailureDoesNotExposeAnEmptyRecoveryState() {
        val viewModel = IrCameraScanViewModel()

        viewModel.onBindFailure(IllegalStateException("   "))

        assertEquals(
            IrCameraUiState.BindFailed("Could not start camera"),
            viewModel.uiState.value,
        )
    }

    private fun frame(): IrPreviewFrame = IrPreviewFrame(
        analysis = IrCameraDetector.FrameAnalysis(
            sources = emptyList(),
            ambientBrightness = 12,
            roomTooBright = false,
            analyzedWidth = 100,
            analyzedHeight = 80,
        ),
        metadata = AnalysisFrameMetadata(
            imageWidth = 100,
            imageHeight = 80,
            crop = IntRect(0, 0, 100, 80),
            rotationDegrees = 0,
            frontCamera = false,
        ),
        previewWidthPx = 300f,
        previewHeightPx = 240f,
    )
}
