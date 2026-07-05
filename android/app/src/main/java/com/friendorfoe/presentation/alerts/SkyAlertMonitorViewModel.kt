package com.friendorfoe.presentation.alerts

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class SkyAlertMonitorViewModel @Inject constructor(
    skyObjectRepository: SkyObjectRepository,
    private val skyAlertNotifier: SkyAlertNotifier
) : ViewModel() {
    init {
        viewModelScope.launch {
            skyObjectRepository.skyObjects.collect { objects ->
                objects.forEach(skyAlertNotifier::notifyObject)
            }
        }
    }
}
