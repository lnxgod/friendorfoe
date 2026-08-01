package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import com.friendorfoe.detection.IrCameraDetector
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

sealed interface IrCameraUiState {
    data object BindingCamera : IrCameraUiState
    data class Live(val frame: IrPreviewFrame) : IrCameraUiState
    data class BindFailed(val message: String) : IrCameraUiState
}

data class IrPreviewFrame(
    val analysis: IrCameraDetector.FrameAnalysis,
    val metadata: AnalysisFrameMetadata,
    val previewWidthPx: Float,
    val previewHeightPx: Float,
    val mappedCentersPx: List<FloatPoint>? = null,
)

@HiltViewModel
class IrCameraScanViewModel @Inject constructor() : ViewModel() {
    private val _uiState = MutableStateFlow<IrCameraUiState>(IrCameraUiState.BindingCamera)
    val uiState: StateFlow<IrCameraUiState> = _uiState.asStateFlow()

    private val _bindingGeneration = MutableStateFlow(0L)
    val bindingGeneration: StateFlow<Long> = _bindingGeneration.asStateFlow()

    fun onFrame(frame: IrPreviewFrame) {
        _uiState.value = IrCameraUiState.Live(frame)
    }

    fun onBindFailure(error: Throwable) {
        val message = error.message?.trim().takeUnless { it.isNullOrEmpty() }
            ?: "Could not start camera"
        _uiState.value = IrCameraUiState.BindFailed(message)
    }

    fun retryBinding() {
        _uiState.value = IrCameraUiState.BindingCamera
        _bindingGeneration.update { it + 1L }
    }
}
