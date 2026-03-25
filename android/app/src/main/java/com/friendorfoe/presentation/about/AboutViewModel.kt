package com.friendorfoe.presentation.about

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

@HiltViewModel
class AboutViewModel @Inject constructor(
    private val detectionPrefs: DetectionPrefs,
    private val skyObjectRepository: SkyObjectRepository
) : ViewModel() {

    val adsbEnabled: Boolean get() = detectionPrefs.adsbEnabled
    val bleRidEnabled: Boolean get() = detectionPrefs.bleRidEnabled
    val wifiEnabled: Boolean get() = detectionPrefs.wifiEnabled
    val privacyEnabled: Boolean get() = detectionPrefs.privacyEnabled
    val stalkerEnabled: Boolean get() = detectionPrefs.stalkerDetectionEnabled

    fun setAdsbEnabled(enabled: Boolean) { detectionPrefs.adsbEnabled = enabled }
    fun setBleRidEnabled(enabled: Boolean) { detectionPrefs.bleRidEnabled = enabled }
    fun setWifiEnabled(enabled: Boolean) { detectionPrefs.wifiEnabled = enabled }
    fun setPrivacyEnabled(enabled: Boolean) {
        skyObjectRepository.setPrivacyDetectionEnabled(enabled)
    }
    fun setStalkerEnabled(enabled: Boolean) { detectionPrefs.stalkerDetectionEnabled = enabled }
}
