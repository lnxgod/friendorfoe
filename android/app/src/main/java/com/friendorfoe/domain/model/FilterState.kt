package com.friendorfoe.domain.model

data class FilterState(
    val searchQuery: String = "",
    val selectedCategories: Set<ObjectCategory> = emptySet(),
    val selectedSources: Set<SourceFilterGroup> = emptySet(),
    val objectTypeFilter: ObjectTypeFilter? = null,
    val maxDistanceNm: Float? = null,
    val minAltitudeFt: Int? = null,
    val maxAltitudeFt: Int? = null,
    val isAdvancedExpanded: Boolean = false
)

enum class SourceFilterGroup { ADS_B, REMOTE_ID, WIFI }

enum class ObjectTypeFilter { AIRCRAFT, DRONE }
