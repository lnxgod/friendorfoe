package com.friendorfoe.presentation.filter

import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.ObjectTypeFilter
import com.friendorfoe.domain.model.SourceFilterGroup
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.domain.model.cleared
import org.junit.Assert.assertEquals
import org.junit.Test

class FilterSummaryTest {
    @Test
    fun activeCountCountsEachNonDefaultDimensionOnce() {
        val filter = FilterState(
            searchQuery = "drone",
            selectedCategories = setOf(ObjectCategory.DRONE, ObjectCategory.MILITARY),
            selectedSources = setOf(SourceFilterGroup.REMOTE_ID, SourceFilterGroup.WIFI),
            objectTypeFilter = ObjectTypeFilter.DRONE,
            maxDistanceNm = 5f,
            minAltitudeFt = 100,
            maxAltitudeFt = 10_000,
            isAdvancedExpanded = true,
        )

        assertEquals(7, activeFilterCount(filter))
        assertEquals(0, activeFilterCount(FilterState()))
    }

    @Test
    fun advancedExpansionDoesNotChangeTheActiveCount() {
        val collapsed = FilterState(maxDistanceNm = 5f)

        assertEquals(
            activeFilterCount(collapsed),
            activeFilterCount(collapsed.copy(isAdvancedExpanded = true)),
        )
    }

    @Test
    fun clearFiltersRestoresEveryDefaultIncludingAdvancedExpansion() {
        val active = FilterState(
            searchQuery = "aircraft",
            selectedCategories = setOf(ObjectCategory.COMMERCIAL),
            selectedSources = setOf(SourceFilterGroup.ADS_B),
            objectTypeFilter = ObjectTypeFilter.AIRCRAFT,
            maxDistanceNm = 10f,
            minAltitudeFt = 500,
            maxAltitudeFt = 20_000,
            isAdvancedExpanded = true,
        )

        assertEquals(FilterState(), active.cleared())
        assertEquals(0, activeFilterCount(active.cleared()))
    }
}
