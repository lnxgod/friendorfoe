package com.friendorfoe.domain.usecase

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectTypeFilter
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.model.SourceFilterGroup

object FilterEngine {

    fun applyFilters(objects: List<SkyObject>, filter: FilterState): List<SkyObject> {
        if (filter == FilterState()) return objects
        return objects.filter { obj -> matchesSkyObject(obj, filter) }
    }

    @JvmName("applyFiltersHistory")
    fun applyFilters(entries: List<HistoryEntity>, filter: FilterState): List<HistoryEntity> {
        if (filter == FilterState()) return entries
        return entries.filter { entry -> matchesHistoryEntity(entry, filter) }
    }

    private fun matchesSkyObject(obj: SkyObject, filter: FilterState): Boolean {
        // Search query
        if (filter.searchQuery.isNotBlank()) {
            val q = filter.searchQuery.lowercase()
            val searchable = when (obj) {
                is Aircraft -> listOfNotNull(
                    obj.callsign, obj.registration, obj.icaoHex,
                    obj.aircraftModel, obj.aircraftType, obj.airline
                )
                is Drone -> listOfNotNull(
                    obj.droneId, obj.manufacturer, obj.model, obj.ssid
                )
            }
            if (searchable.none { it.lowercase().contains(q) }) return false
        }

        // Category filter
        if (filter.selectedCategories.isNotEmpty() && obj.category !in filter.selectedCategories) {
            return false
        }

        // Source filter
        if (filter.selectedSources.isNotEmpty()) {
            val group = sourceToGroup(obj.source) ?: return false
            if (group !in filter.selectedSources) return false
        }

        // Object type filter
        if (filter.objectTypeFilter != null) {
            val matches = when (filter.objectTypeFilter) {
                ObjectTypeFilter.AIRCRAFT -> obj is Aircraft
                ObjectTypeFilter.DRONE -> obj is Drone
            }
            if (!matches) return false
        }

        // Distance filter (NM)
        if (filter.maxDistanceNm != null && obj.distanceMeters != null) {
            val nm = obj.distanceMeters!! / 1852.0
            if (nm > filter.maxDistanceNm) return false
        }

        // Altitude filter (feet)
        val altFt = (obj.position.altitudeMeters * 3.281).toInt()
        if (filter.minAltitudeFt != null && altFt < filter.minAltitudeFt) return false
        if (filter.maxAltitudeFt != null && altFt > filter.maxAltitudeFt) return false

        return true
    }

    private fun matchesHistoryEntity(entry: HistoryEntity, filter: FilterState): Boolean {
        // Search query
        if (filter.searchQuery.isNotBlank()) {
            val q = filter.searchQuery.lowercase()
            val searchable = listOfNotNull(
                entry.objectId, entry.displayName, entry.description, entry.category
            )
            if (searchable.none { it.lowercase().contains(q) }) return false
        }

        // Category filter
        if (filter.selectedCategories.isNotEmpty()) {
            val entryCategory = try {
                com.friendorfoe.domain.model.ObjectCategory.valueOf(entry.category.uppercase())
            } catch (_: IllegalArgumentException) {
                com.friendorfoe.domain.model.ObjectCategory.UNKNOWN
            }
            if (entryCategory !in filter.selectedCategories) return false
        }

        // Source filter
        if (filter.selectedSources.isNotEmpty()) {
            val group = historySourceToGroup(entry.detectionSource) ?: return false
            if (group !in filter.selectedSources) return false
        }

        // Object type filter
        if (filter.objectTypeFilter != null) {
            val matches = when (filter.objectTypeFilter) {
                ObjectTypeFilter.AIRCRAFT -> entry.objectType.equals("aircraft", ignoreCase = true)
                ObjectTypeFilter.DRONE -> entry.objectType.equals("drone", ignoreCase = true)
            }
            if (!matches) return false
        }

        // Distance filter (NM)
        if (filter.maxDistanceNm != null && entry.distanceMeters != null) {
            val nm = entry.distanceMeters / 1852.0
            if (nm > filter.maxDistanceNm) return false
        }

        // Altitude filter (feet)
        val altFt = (entry.altitudeMeters * 3.281).toInt()
        if (filter.minAltitudeFt != null && altFt < filter.minAltitudeFt) return false
        if (filter.maxAltitudeFt != null && altFt > filter.maxAltitudeFt) return false

        return true
    }

    private fun sourceToGroup(source: DetectionSource): SourceFilterGroup? = when (source) {
        DetectionSource.ADS_B -> SourceFilterGroup.ADS_B
        DetectionSource.REMOTE_ID -> SourceFilterGroup.REMOTE_ID
        DetectionSource.WIFI_NAN, DetectionSource.WIFI_BEACON, DetectionSource.WIFI -> SourceFilterGroup.WIFI
    }

    private fun historySourceToGroup(source: String): SourceFilterGroup? = when (source.lowercase()) {
        "ads_b" -> SourceFilterGroup.ADS_B
        "remote_id" -> SourceFilterGroup.REMOTE_ID
        "wifi_nan", "wifi_beacon", "wifi" -> SourceFilterGroup.WIFI
        else -> null
    }
}
