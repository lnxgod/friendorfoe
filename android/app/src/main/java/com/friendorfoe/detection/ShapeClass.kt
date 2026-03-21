package com.friendorfoe.detection

/** Inferred physical shape of a visually detected drone or aircraft. */
enum class ShapeClass(val label: String) {
    QUADCOPTER("Quadcopter"),
    FIXED_WING("Fixed-wing"),
    DELTA_WING("Delta-wing"),
    MULTIROTOR("Multirotor"),
    HELICOPTER("Helicopter"),
    INDETERMINATE("Unknown shape")
}
