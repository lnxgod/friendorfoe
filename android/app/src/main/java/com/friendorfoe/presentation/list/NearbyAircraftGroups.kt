package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.AircraftRange
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject

internal enum class NearbyDistanceSection(val key: String) {
    WITHIN_RANGE("nearby"), FARTHER_AWAY("farther"), UNKNOWN("unknown");

    fun label(rangeMiles: Int): String = when (this) {
        WITHIN_RANGE -> "Within ${AircraftRange.normalizeMiles(rangeMiles)} mi"
        FARTHER_AWAY -> "Farther away"
        UNKNOWN -> "Distance unknown"
    }
}

internal enum class NearbyAircraftType(val label: String) {
    HELICOPTERS("Helicopters"),
    DRONES("Drones"),
    COMMERCIAL("Commercial aircraft"),
    GENERAL_AVIATION("General aviation"),
    MILITARY("Military aircraft"),
    PUBLIC_SAFETY("Public safety aircraft"),
    GOVERNMENT("Government aircraft"),
    CARGO("Cargo aircraft"),
    GROUND_VEHICLES("Ground vehicles"),
    OTHER("Other aircraft"),
}

internal data class NearbyTypeGroup(val type: NearbyAircraftType, val rows: List<SkyObject>) {
    val nearestDistance: Double = rows.minOf(SkyObject::listSortDistance)
}

internal data class NearbyAircraftSection(
    val section: NearbyDistanceSection,
    val rows: List<SkyObject>,
    val groups: List<NearbyTypeGroup>,
)

internal fun SkyObject.nearbyAircraftType(): NearbyAircraftType = when {
    this is Drone || category == ObjectCategory.DRONE -> NearbyAircraftType.DRONES
    category == ObjectCategory.GROUND_VEHICLE -> NearbyAircraftType.GROUND_VEHICLES
    this is Aircraft && isRotorcraft(this) -> NearbyAircraftType.HELICOPTERS
    category == ObjectCategory.MILITARY -> NearbyAircraftType.MILITARY
    category == ObjectCategory.EMERGENCY || listBadgeText(this) != null -> NearbyAircraftType.PUBLIC_SAFETY
    category == ObjectCategory.GOVERNMENT -> NearbyAircraftType.GOVERNMENT
    category == ObjectCategory.COMMERCIAL -> NearbyAircraftType.COMMERCIAL
    category == ObjectCategory.GENERAL_AVIATION -> NearbyAircraftType.GENERAL_AVIATION
    category == ObjectCategory.CARGO -> NearbyAircraftType.CARGO
    else -> NearbyAircraftType.OTHER
}

internal fun groupNearbyAircraft(rows: List<SkyObject>, rangeMiles: Int): List<NearbyAircraftSection> {
    val byRange = sortSkyObjectsForList(rows).groupBy {
        when {
            !it.listSortDistance().isFinite() -> NearbyDistanceSection.UNKNOWN
            AircraftRange.contains(it.distanceMeters, rangeMiles) -> NearbyDistanceSection.WITHIN_RANGE
            else -> NearbyDistanceSection.FARTHER_AWAY
        }
    }
    return NearbyDistanceSection.entries.mapNotNull { section ->
        val members = byRange[section].orEmpty()
        // Always make it clear when all reported traffic is outside the chosen range.
        if (members.isEmpty() && section != NearbyDistanceSection.WITHIN_RANGE) return@mapNotNull null
        val groups = members.groupBy(SkyObject::nearbyAircraftType).map { (type, findings) -> NearbyTypeGroup(type, findings) }
            .sortedWith(compareBy<NearbyTypeGroup> { it.nearestDistance }.thenBy { it.type.ordinal })
        NearbyAircraftSection(section, members, groups)
    }
}

internal fun nearbyListCountLabel(rows: List<SkyObject>, rangeMiles: Int): String {
    val nearby = rows.count { AircraftRange.contains(it.distanceMeters, rangeMiles) }
    val unknown = rows.count { !it.listSortDistance().isFinite() }
    val farther = rows.size - nearby - unknown
    return buildList {
        add("$nearby within ${AircraftRange.normalizeMiles(rangeMiles)} mi")
        if (farther > 0) add("$farther farther away")
        if (unknown > 0) add("$unknown distance unknown")
    }.joinToString(" · ")
}
