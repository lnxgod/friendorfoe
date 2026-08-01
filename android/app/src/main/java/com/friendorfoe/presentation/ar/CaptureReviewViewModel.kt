package com.friendorfoe.presentation.ar

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

@HiltViewModel
class CaptureReviewViewModel @Inject constructor(
    private val writer: PhotoWriter,
    private val shareFactory: ShareImageFactory,
) : ViewModel() {
    private val _state = MutableStateFlow<CaptureReviewState>(CaptureReviewState.Empty)
    val state = _state.asStateFlow()

    private val _effects = MutableSharedFlow<CaptureReviewEffect>(extraBufferCapacity = 1)
    val effects = _effects.asSharedFlow()
    private var saveJob: Job? = null
    private var reviewGeneration = 0L

    fun inspect(draft: CaptureDraft) {
        saveJob?.cancel()
        reviewGeneration += 1
        _state.value = CaptureReviewState.Reviewing(draft)
    }

    fun discard() {
        saveJob?.cancel()
        reviewGeneration += 1
        _state.value = CaptureReviewState.Empty
    }

    fun share(): Job? {
        val draft = currentDraft() ?: return null
        val generation = reviewGeneration
        return viewModelScope.launch {
            val result = try {
                shareFactory.create(draft)
            } catch (cancellation: CancellationException) {
                throw cancellation
            } catch (failure: Throwable) {
                Result.failure(failure)
            }
            result.fold(
                onSuccess = { request ->
                    if (reviewGeneration == generation && currentDraft() == draft) {
                        _effects.emit(CaptureReviewEffect.LaunchShare(request))
                    }
                },
                onFailure = { failure ->
                    if (failure is CancellationException) throw failure
                    if (reviewGeneration == generation && currentDraft() == draft) {
                        _state.value = CaptureReviewState.ShareFailed(
                            draft,
                            "Could not prepare photo to share.",
                        )
                    }
                },
            )
        }
    }

    fun save(): Job? {
        if (saveJob?.isActive == true) return null
        val draft = currentDraft() ?: return null
        _state.value = CaptureReviewState.Saving(draft)
        return viewModelScope.launch {
            try {
                val result = try {
                    writer.write(draft)
                } catch (cancellation: CancellationException) {
                    throw cancellation
                } catch (failure: Throwable) {
                    Result.failure(failure)
                }
                val next = result.fold(
                    onSuccess = CaptureReviewState::Saved,
                    onFailure = { failure ->
                        if (failure is CancellationException) throw failure
                        CaptureReviewState.SaveFailed(draft, "Could not save photo.")
                    },
                )
                if (_state.value == CaptureReviewState.Saving(draft)) {
                    _state.value = next
                }
            } catch (cancellation: CancellationException) {
                if (_state.value == CaptureReviewState.Saving(draft)) {
                    _state.value = CaptureReviewState.Reviewing(draft)
                }
                throw cancellation
            }
        }.also {
            saveJob = it
        }
    }

    fun retrySave(): Job? = if (_state.value is CaptureReviewState.SaveFailed) save() else null

    private fun currentDraft(): CaptureDraft? = when (val value = _state.value) {
        is CaptureReviewState.Reviewing -> value.draft
        is CaptureReviewState.SaveFailed -> value.draft
        is CaptureReviewState.ShareFailed -> value.draft
        else -> null
    }
}
