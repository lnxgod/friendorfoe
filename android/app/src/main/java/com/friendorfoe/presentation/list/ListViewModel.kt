package com.friendorfoe.presentation.list

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.SkyObject
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import javax.inject.Inject

/**
 * ViewModel for the List View screen.
 *
 * Exposes sky objects from [SkyObjectRepository] sorted by distance
 * from the user (nearest first). Objects without a known distance
 * are placed at the end of the list.
 */
@HiltViewModel
class ListViewModel @Inject constructor(
    skyObjectRepository: SkyObjectRepository
) : ViewModel() {

    /** All detected sky objects sorted by distance (nearest first). */
    val skyObjects: StateFlow<List<SkyObject>> = skyObjectRepository.skyObjects
        .map { objects ->
            objects.sortedBy { it.distanceMeters ?: Double.MAX_VALUE }
        }
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5000),
            initialValue = emptyList()
        )
}
