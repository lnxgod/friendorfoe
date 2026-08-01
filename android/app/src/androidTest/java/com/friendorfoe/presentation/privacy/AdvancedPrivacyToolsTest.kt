package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.Modifier
import com.friendorfoe.detection.FloatPoint
import com.friendorfoe.detection.IrCameraDetector
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class AdvancedPrivacyToolsTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun magneticInitialStateDoesNotInventReading() {
        compose.setContent {
            MaterialTheme {
                MagneticFieldContent(
                    state = MagneticFieldUiState.Initializing,
                    actions = MagneticFieldActions(),
                )
            }
        }

        compose.onNodeWithText("Waiting for a reliable magnetometer sample").assertIsDisplayed()
        compose.onNodeWithText("0 µT", substring = true).assertDoesNotExist()
        compose.onNodeWithText("NORMAL", substring = true).assertDoesNotExist()
    }

    @Test
    fun magneticUnavailableStateExplainsLimitationAndOffersRetry() {
        compose.setContent {
            MaterialTheme {
                MagneticFieldContent(
                    state = MagneticFieldUiState.SensorUnavailable,
                    actions = MagneticFieldActions(),
                )
            }
        }

        compose.onNodeWithText("Magnetometer unavailable").assertIsDisplayed()
        compose.onNodeWithText("Retry").assertHasClickAction()
    }

    @Test
    fun irResultUsesCautiousLanguageAndKeepsMarkerInsidePreview() {
        compose.setContent {
            MaterialTheme {
                IrCameraContent(
                    state = irStateWithBrightPoint(),
                    actions = IrActions(),
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }

        compose.onNodeWithText("Possible IR-like light", substring = true).assertIsDisplayed()
        compose.onNodeWithText("Camera detected", substring = true).assertDoesNotExist()
        val previewBounds = compose.onNodeWithTag("ir_preview").fetchSemanticsNode().boundsInRoot
        val markerBounds = compose.onNodeWithTag("ir_source_0").fetchSemanticsNode().boundsInRoot
        assertTrue(previewBounds.contains(markerBounds.center))
    }

    @Test
    fun bindFailureShowsRetryWithoutPreview() {
        compose.setContent {
            MaterialTheme {
                IrCameraContent(
                    state = IrCameraUiState.BindFailed("Could not start camera"),
                    actions = IrActions(),
                )
            }
        }

        compose.onNodeWithText("Retry").assertHasClickAction()
        compose.onNodeWithTag("ir_preview").assertDoesNotExist()
        compose.onNodeWithText("Grant Camera").assertDoesNotExist()
    }

    private fun irStateWithBrightPoint(): IrCameraUiState = IrCameraUiState.Live(
        frame = IrPreviewFrame(
            analysis = IrCameraDetector.FrameAnalysis(
                sources = listOf(
                    IrCameraDetector.IrSource(
                        centerPx = FloatPoint(50f, 50f),
                        brightness = 245,
                        clusterSize = 8,
                        confidence = 0.8f,
                    ),
                ),
                ambientBrightness = 18,
                roomTooBright = false,
                analyzedWidth = 100,
                analyzedHeight = 100,
            ),
            metadata = AnalysisFrameMetadata(
                imageWidth = 100,
                imageHeight = 100,
                crop = IntRect(10, 10, 90, 90),
                rotationDegrees = 90,
                frontCamera = true,
            ),
            previewWidthPx = 300f,
            previewHeightPx = 300f,
        ),
    )
}
