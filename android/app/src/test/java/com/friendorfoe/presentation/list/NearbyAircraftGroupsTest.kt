package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.AircraftRange
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.*
import org.junit.Test
import java.time.Instant

class NearbyAircraftGroupsTest {
    @Test
    fun rangePartitionsEveryObservationExactlyOnceIncludingInvalidDistances() {
        val rows = listOf(plane("HERE", 0.0), plane("EDGE", 10.0), plane("OUT", 10.0001),
            plane("FAR_HELI", 50.0, ObjectCategory.HELICOPTER), plane("MISSING", null),
            plane("NAN", Double.NaN), plane("NEGATIVE", -1.0), plane("INFINITY", Double.POSITIVE_INFINITY))
        val groups = groupNearbyAircraft(rows, 10)
        assertEquals(listOf("HERE", "EDGE"), groups[0].rows.map { it.id })
        assertEquals(listOf("OUT", "FAR_HELI"), groups[1].rows.map { it.id })
        assertEquals(setOf("MISSING", "NAN", "NEGATIVE", "INFINITY"), groups[2].rows.map { it.id }.toSet())
        assertEquals(rows.size, groups.sumOf { it.rows.size })
        assertEquals(rows.map { it.id }.toSet(), groups.flatMap { it.groups }.flatMap { it.rows }.map { it.id }.toSet())
    }

    @Test
    fun groupOrderUsesClosestMemberAndEachGroupSortsByDistance() {
        val rows = listOf(plane("HELI9", 9.0, ObjectCategory.HELICOPTER), plane("JET2", 2.0),
            plane("HELI4", 4.0, ObjectCategory.HELICOPTER), plane("JET1", 1.0))
        val nearby = groupNearbyAircraft(rows, 10).first()
        assertEquals(listOf(NearbyAircraftType.COMMERCIAL, NearbyAircraftType.HELICOPTERS), nearby.groups.map { it.type })
        assertEquals(listOf("JET1", "JET2"), nearby.groups[0].rows.map { it.id })
        assertEquals(listOf("HELI4", "HELI9"), nearby.groups[1].rows.map { it.id })
        assertEquals(nearby, groupNearbyAircraft(rows.reversed(), 10).first())
    }

    @Test
    fun sheriffAndMilitaryRotorcraftBelongToHelicoptersWithoutDuplicateGroups() {
        val sheriff = plane("SHERIFF", 3.0, ObjectCategory.GOVERNMENT).copy(aircraftType = "AS50",
            operatorName = "County Sheriff", classificationSignals = listOf("OWNER:PUBLIC_SAFETY"))
        val military = plane("ARMY", 4.0, ObjectCategory.MILITARY).copy(aircraftType = "H60")
        val groups = groupNearbyAircraft(listOf(sheriff, military), 10).first().groups
        assertEquals(NearbyAircraftType.HELICOPTERS, groups.single().type)
        assertEquals(listOf(sheriff, military), groups.single().rows)
        assertEquals("Law enforcement", listAttentionLabel(sheriff))
        assertEquals("Military", listAttentionLabel(military))
    }

    @Test
    fun rangeChangeMovesExistingHelicopterWithoutPromotingItAboveCloserPlane() {
        val plane = plane("PLANE", 1.0)
        val helicopter = plane("HELI", 12.0, ObjectCategory.HELICOPTER)
        val ten = groupNearbyAircraft(listOf(helicopter, plane), 10)
        assertEquals(listOf(plane), ten[0].rows)
        assertEquals(listOf(helicopter), ten[1].rows)
        val fifteen = groupNearbyAircraft(listOf(helicopter, plane), 15)
        assertEquals(listOf(plane, helicopter), fifteen.single().rows)
        assertEquals(NearbyAircraftType.COMMERCIAL, fifteen.single().groups.first().type)
    }

    @Test
    fun allDistantTrafficStillShowsAnEmptyNearbySection() {
        val sections = groupNearbyAircraft(listOf(plane("FAR", 50.0)), 10)
        assertEquals(NearbyDistanceSection.WITHIN_RANGE, sections.first().section)
        assertTrue(sections.first().rows.isEmpty())
        assertEquals(NearbyDistanceSection.FARTHER_AWAY, sections.last().section)
    }

    @Test
    fun unknownDistancesNeverImplyNearbyAndTypesStayDeterministic() {
        val helicopter = plane("HELI", null, ObjectCategory.HELICOPTER)
        val jet = plane("JET", null)
        val sections = groupNearbyAircraft(listOf(jet, helicopter), 10)
        assertTrue(sections.first().rows.isEmpty())
        assertEquals(NearbyDistanceSection.UNKNOWN, sections.last().section)
        assertTrue(sections.last().groups.all { !it.nearestDistance.isFinite() })
        assertEquals(sections, groupNearbyAircraft(listOf(helicopter, jet), 10))
    }

    @Test
    fun headerSeparatesDistantAndUnknownTrafficFromNearbyCount() {
        assertEquals("1 within 10 mi · 1 farther away · 1 distance unknown",
            nearbyListCountLabel(listOf(plane("NEAR", 1.0), plane("FAR", 50.0), plane("UNKNOWN", null)), 10))
        assertEquals("0 within 10 mi", nearbyListCountLabel(emptyList(), 10))
    }

    private fun plane(id: String, miles: Double?, category: ObjectCategory = ObjectCategory.COMMERCIAL) = Aircraft(
        id = id, icaoHex = id, callsign = id, category = category, position = Position(32.7, -117.1, 1000.0),
        firstSeen = Instant.EPOCH, lastUpdated = Instant.EPOCH, distanceMeters = miles?.times(AircraftRange.METERS_PER_MILE),
    )
}
