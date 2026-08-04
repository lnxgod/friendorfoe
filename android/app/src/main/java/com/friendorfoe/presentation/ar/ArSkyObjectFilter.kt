package com.friendorfoe.presentation.ar

import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.model.isFormationDroneId

/**
 * Formation pixels are a map-only presentation. Keeping them out of this list
 * avoids projecting and correlating hundreds of stationary points every AR frame.
 */
internal fun List<SkyObject>.withoutFormationDrones(): List<SkyObject> =
    filterNot { it is Drone && isFormationDroneId(it.droneId) }
