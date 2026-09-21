package com.friendorfoe.presentation.alerts

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.combine
import javax.inject.Inject

@HiltViewModel
class SkyAlertMonitorViewModel @Inject constructor(
    skyObjectRepository: SkyObjectRepository,
    private val skyAlertNotifier: SkyAlertNotifier,
    detectionPrefs: DetectionPrefs,
) : ViewModel() {
    init {
        viewModelScope.launch {
            combine(skyObjectRepository.skyObjects, detectionPrefs.settings) { objects, _ ->
                objects
            }.collect { objects ->
                objects.forEach(skyAlertNotifier::notifyObject)
            }
        }
    }
}
