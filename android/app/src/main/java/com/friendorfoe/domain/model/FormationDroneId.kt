package com.friendorfoe.domain.model

private const val FORMATION_DRONE_ID_PREFIX = "FOF-C5-"

/** Identifies local C5 formation pixels across repository and presentation policy. */
fun isFormationDroneId(droneId: String): Boolean =
    droneId.startsWith(FORMATION_DRONE_ID_PREFIX)
