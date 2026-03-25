package com.friendorfoe.presentation.about

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.GlassesDetectionPrefs
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

@HiltViewModel
class AboutViewModel @Inject constructor(
    private val glassesPrefs: GlassesDetectionPrefs,
    private val skyObjectRepository: SkyObjectRepository
) : ViewModel() {

    val isGlassesDetectionEnabled: Boolean get() = glassesPrefs.isEnabled

    fun setGlassesDetectionEnabled(enabled: Boolean) {
        skyObjectRepository.setGlassesDetectionEnabled(enabled)
    }
}
