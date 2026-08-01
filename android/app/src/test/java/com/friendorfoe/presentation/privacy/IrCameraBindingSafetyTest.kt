package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.IrCameraDetector
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class IrCameraBindingSafetyTest {
    @Test
    fun claimingFailureInvalidatesGenerationBeforeQueuedFrameCanPublish() {
        val gate = IrBindingGenerationGate()
        val generation = gate.beginGeneration()
        val viewModel = IrCameraScanViewModel()

        assertTrue(gate.claimFailure(generation))
        viewModel.onBindFailure(IllegalStateException("Could not analyze camera frame"))
        if (gate.isActive(generation)) {
            viewModel.onFrame(frame())
        }

        assertFalse(gate.isActive(generation))
        assertFalse(gate.claimFailure(generation))
        assertEquals(
            IrCameraUiState.BindFailed("Could not analyze camera frame"),
            viewModel.uiState.value,
        )
    }

    @Test
    fun missingConvertedBitmapIsAnExplicitFrameFailure() {
        val failure = assertThrows(IllegalStateException::class.java) {
            requireConvertedFrame(null)
        }

        assertEquals("Could not analyze camera frame", failure.message)
    }

    private fun frame(): IrPreviewFrame = IrPreviewFrame(
        analysis = IrCameraDetector.FrameAnalysis(
            sources = emptyList(),
            ambientBrightness = 0,
            roomTooBright = false,
            analyzedWidth = 10,
            analyzedHeight = 10,
        ),
        metadata = AnalysisFrameMetadata(
            imageWidth = 10,
            imageHeight = 10,
            crop = IntRect(0, 0, 10, 10),
            rotationDegrees = 0,
            frontCamera = false,
        ),
        previewWidthPx = 10f,
        previewHeightPx = 10f,
    )
}
