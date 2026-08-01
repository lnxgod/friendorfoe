package com.friendorfoe.presentation.reference

import com.friendorfoe.presentation.navigation.encodeRouteSegment

enum class ReferenceTab(val wireValue: String) {
    AIRCRAFT("aircraft"),
    DRONES("drones"),
}

fun referenceGuideRoute(
    tab: ReferenceTab = ReferenceTab.AIRCRAFT,
    query: String = "",
): String = "reference_guide?tab=${encodeRouteSegment(tab.wireValue)}" +
    "&query=${encodeRouteSegment(query)}"
