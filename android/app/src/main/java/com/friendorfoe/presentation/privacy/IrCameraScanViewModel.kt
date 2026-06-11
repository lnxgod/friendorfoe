package com.friendorfoe.presentation.privacy

import android.graphics.Bitmap
import androidx.lifecycle.ViewModel
import com.friendorfoe.detection.IrCameraDetector
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

data class IrCameraScanUiState(
    val sources: List<IrCameraDetector.IrSource> = emptyList(),
    val framesAnalyzed: Long = 0,
    val peakCount: Int = 0
)

@HiltViewModel
class IrCameraScanViewModel @Inject constructor(
    private val irCameraDetector: IrCameraDetector
) : ViewModel() {
    private val _uiState = MutableStateFlow(IrCameraScanUiState())
    val uiState: StateFlow<IrCameraScanUiState> = _uiState.asStateFlow()

    fun analyzeFrame(bitmap: Bitmap) {
        val sources = irCameraDetector.analyzeFrame(bitmap)
        _uiState.update { state ->
            state.copy(
                sources = sources,
                framesAnalyzed = state.framesAnalyzed + 1,
                peakCount = maxOf(state.peakCount, sources.size)
            )
        }
    }

    fun reset() {
        irCameraDetector.reset()
        _uiState.value = IrCameraScanUiState()
    }
}
