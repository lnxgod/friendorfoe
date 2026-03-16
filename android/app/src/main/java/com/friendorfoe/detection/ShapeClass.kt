package com.friendorfoe.detection

/** Inferred physical shape of a visually detected drone or aircraft. */
enum class ShapeClass(val label: String) {
    QUADCOPTER("Quadcopter"),
    FIXED_WING("Fixed-wing"),
    MULTIROTOR("Multirotor"),
    HELICOPTER("Helicopter"),
    INDETERMINATE("Unknown shape")
}
