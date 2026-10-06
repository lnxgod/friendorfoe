package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyDeviceGroupsTest {
    @Test fun recorderFirstDefaultAndSavedTabRestoreAreExplicit() {
        assertEquals(PrivacyFocus.RECORDERS, restoredPrivacyFilters(null).focus)
        assertEquals(PrivacyFocus.RECORDERS, restoredPrivacyFilters("invalid").focus)
        assertEquals(PrivacyFocus.BEACONS, restoredPrivacyFilters("BEACONS").focus)
    }

    @Test fun clearingRefinementsKeepsTheSelectedTab() {
        val filters = PrivacyFilterState(focus = PrivacyFocus.RECORDERS, query = "Plaud",
            sources = setOf(PrivacySourceKind.PHONE_BLE), liveOnly = true)
        assertEquals(3, filters.refinementCount)
        assertEquals(PrivacyFilterState(focus = PrivacyFocus.RECORDERS), filters.clearRefinements())
    }

    @Test fun tabCountsStayVisibleWhenSearchHidesAllResults() {
        val recorder = finding(id = "plaud", title = "Plaud", category = PrivacyCategory.VOICE_RECORDER)
        val beacon = finding(id = "beacon", category = PrivacyCategory.VENUE_BEACON)
        val projected = projectPrivacyUiState(current(listOf(recorder, beacon)),
            PrivacyFilterState(focus = PrivacyFocus.RECORDERS, query = "missing"))
        assertTrue(projected.visibleFindings.isEmpty())
        assertEquals(mapOf(PrivacyFocus.RECORDERS to 1, PrivacyFocus.QUIET to 1,
            PrivacyFocus.BEACONS to 1, PrivacyFocus.ALL to 2), projected.focusCounts)
    }

    @Test fun recorderAndQuietFiltersKeepWarningsAndCombineWithSearch() {
        val recorder = finding(id = "plaud", title = "AI Voice Recorder", evidence = "Plaud", category = PrivacyCategory.VOICE_RECORDER, severity = FindingSeverity.AWARENESS)
        val beacon = finding(id = "beacon", title = "iBeacon", category = PrivacyCategory.VENUE_BEACON)
        val warning = beacon.copy(displayId = "warning", severity = FindingSeverity.CRITICAL)
        val rows = current(listOf(recorder, beacon, warning))
        assertEquals(listOf(recorder), projectPrivacyUiState(rows, PrivacyFilterState(focus = PrivacyFocus.RECORDERS, query = "Plaud")).visibleFindings)
        assertEquals(listOf(recorder, warning), projectPrivacyUiState(rows, PrivacyFilterState(focus = PrivacyFocus.QUIET)).visibleFindings)
        assertEquals(listOf(beacon, warning), projectPrivacyUiState(rows, PrivacyFilterState(focus = PrivacyFocus.BEACONS)).visibleFindings)
        assertEquals(3, projectPrivacyUiState(rows).visibleFindings.size)
    }

    @Test fun iBeaconNetworksGroupByUuidWithoutMergingObservations() {
        val uuid = "00112233-4455-6677-8899-AABBCCDDEEFF"
        val a = finding(id = "a", title = "iBeacon", category = PrivacyCategory.VENUE_BEACON).copy(beaconUuid = uuid)
        val b = a.copy(displayId = "b", observationKey = PrivacyFindingKey(a.source, "b"), beaconUuid = uuid.lowercase())
        val unknown = a.copy(displayId = "unknown", beaconUuid = null)
        val family = groupPrivacyDevices(listOf(a, b, unknown)).single().families.single()
        assertEquals(2, family.networks.size)
        assertEquals(listOf(a, b), family.networks.first().findings)
        assertEquals(listOf(unknown), family.networks.last().findings)
        assertEquals(listOf(a, b), projectPrivacyUiState(current(listOf(a, b, unknown)), PrivacyFilterState(query = uuid)).visibleFindings)
    }

    @Test
    fun existingProtocolAndManufacturerLabelsSelectTheirFamilies() {
        val cases = listOf(
            Triple("AirTag (Separated)", "Apple • airtag_separated", PrivacyDeviceFamily.FIND_MY),
            Triple("Tracker", "find_my accessory", PrivacyDeviceFamily.FIND_MY),
            Triple("Find Hub accessory", "normal advertising mode", PrivacyDeviceFamily.FIND_HUB),
            Triple("BLE Tracker", "Tile • uuid:0xfeed", PrivacyDeviceFamily.TILE),
            Triple("BLE Tracker", "Samsung • uuid:0xfd5a", PrivacyDeviceFamily.SMART_TAG),
            Triple("SmartTag", "Nearby", PrivacyDeviceFamily.SMART_TAG),
            Triple("BLE Tracker", "Chipolo • manufacturer", PrivacyDeviceFamily.CHIPOLO),
            Triple("BLE Tracker", "Nearby signal", PrivacyDeviceFamily.OTHER_TRACKERS),
        )
        cases.forEach { (title, evidence, expected) ->
            assertEquals(title, expected, finding(title = title, evidence = evidence).deviceFamily())
        }
        // Find My is a network family, not a guarantee that the accessory is an AirTag.
        assertEquals(PrivacyDeviceFamily.FIND_MY, finding(category = PrivacyCategory.FINDMY, title = "Accessory").deviceFamily())
        assertEquals(PrivacyDeviceFamily.FIND_MY, finding(title = "Chipolo Find My accessory").deviceFamily())
        assertEquals(PrivacyDeviceFamily.FIND_HUB, finding(title = "Chipolo Find Hub accessory").deviceFamily())
    }

    @Test
    fun beaconsUseProtocolChildrenAndUnknownLabelsStayUnknown() {
        val beacon = finding(category = PrivacyCategory.VENUE_BEACON)
        assertEquals(PrivacyDeviceFamily.IBEACON, beacon.copy(title = "iBeacon Lobby").deviceFamily())
        assertEquals(PrivacyDeviceFamily.EDDYSTONE, beacon.copy(title = "Eddystone Beacon").deviceFamily())
        assertEquals(PrivacyDeviceFamily.OTHER_BEACONS, beacon.copy(title = "Venue Beacon").deviceFamily())
        assertEquals(PrivacyDeviceFamily.OTHER_TRACKERS, finding(title = "Versatile tracker").deviceFamily())
        assertEquals(PrivacyDeviceFamily.OTHER_TRACKERS, finding(title = "Accessory", limitation = "Could be an AirTag or Tile").deviceFamily())
        assertNull(finding(category = PrivacyCategory.SMART_TV, title = "Samsung").deviceFamily())
        assertNull(finding(category = PrivacyCategory.APPLE_CONTINUITY, title = "Apple").deviceFamily())
    }

    @Test
    fun allActionableFindingsStayOutsideCollapsedTree() {
        val rows = listOf(PrivacyCategory.FINDMY, PrivacyCategory.BLE_TRACKER, PrivacyCategory.ATTACK_TOOL).flatMap { category ->
            listOf(FindingSeverity.AWARENESS, FindingSeverity.CRITICAL).map { severity ->
                finding(category = category, severity = severity, title = "AirTag", id = "$category-$severity")
            }
        }
        val state = projectPrivacyUiState(current(rows))
        assertTrue(groupPrivacyDevices(rows).isEmpty())
        assertEquals(rows, state.individualFindings)
    }

    @Test
    fun everyObservationAppearsExactlyOnceWithoutCombiningSourcesOrNames() {
        val phone = finding(id = "same", title = "iBeacon", category = PrivacyCategory.VENUE_BEACON)
        val backendKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, phone.observationKey.sourceRecordId)
        val backend = phone.copy(source = PrivacySourceKind.BACKEND, observationKey = backendKey, routableKey = backendKey)
        val tracker = finding(id = "tracker", title = "Tile")
        val warning = finding(id = "warning", severity = FindingSeverity.CRITICAL)
        val rows = listOf(phone, backend, tracker, warning)
        val state = projectPrivacyUiState(current(rows))
        val grouped = groupPrivacyDevices(rows)
        val rendered = state.individualFindings + grouped.flatMap { it.findings }
        assertEquals(rows.size, rendered.size)
        assertEquals(rows.toSet(), rendered.toSet())
        assertEquals(listOf(phone, backend), grouped.last().families.single().findings)
        assertEquals(backend, projectPrivacyUiState(current(rows), focusedKey = backendKey).focusedFinding)
    }

    @Test
    fun familyOrderAndLeafOrderRemainStableWhenSignalsChange() {
        val rows = listOf(finding(id = "tile", title = "Tile"), finding(id = "findmy", title = "AirTag"), finding(id = "tile2", title = "Tile"))
        val branches = groupPrivacyDevices(rows).single().families
        assertEquals(listOf(PrivacyDeviceFamily.FIND_MY, PrivacyDeviceFamily.TILE), branches.map { it.family })
        assertEquals(listOf("tile", "tile2"), branches.last().findings.map { it.displayId })
        assertEquals(branches.map { it.findings.map { row -> row.observationKey } },
            groupPrivacyDevices(rows.map { it.copy(signalDbm = -20) }).single().families.map { it.findings.map { row -> row.observationKey } })
    }

    @Test
    fun familySearchSourceAndFreshnessFiltersApplyBeforeGrouping() {
        val live = finding(id = "live", title = "BLE Tracker", evidence = "Samsung • uuid:fd5a")
        val stale = live.copy(displayId = "stale", observationKey = PrivacyFindingKey(live.source, "stale"), freshness = FindingFreshness.STALE)
        val beacon = finding(id = "beacon", category = PrivacyCategory.VENUE_BEACON, title = "iBeacon")
        val state = projectPrivacyUiState(current(listOf(live, stale, beacon)),
            PrivacyFilterState(query = "smarttag", liveOnly = true, sources = setOf(PrivacySourceKind.PHONE_BLE)))
        assertEquals(listOf(live), groupPrivacyDevices(state.visibleFindings).single().findings)
        assertEquals(2, projectPrivacyUiState(current(listOf(live, stale, beacon)), PrivacyFilterState(query = "trackers")).visibleFindings.size)
        assertTrue(groupPrivacyDevices(projectPrivacyUiState(current(listOf(live)), PrivacyFilterState(attentionOnly = true)).visibleFindings).isEmpty())
    }

    @Test
    fun familyNameSearchStillShowsActionableTrackerEvidence() {
        val warning = finding(title = "Repeated tracker encounter", evidence = "Samsung", severity = FindingSeverity.AWARENESS)
        val state = projectPrivacyUiState(current(listOf(warning)), PrivacyFilterState(query = "SmartTag", attentionOnly = true))
        assertEquals(listOf(warning), state.individualFindings)
        assertTrue(groupPrivacyDevices(state.visibleFindings).isEmpty())
    }

    private fun current(rows: List<PrivacyFinding>) = PrivacyCurrentState(emptyList(), rows, 0, emptyList(), true)

    private fun finding(
        id: String = "row",
        title: String = "Unknown",
        evidence: String? = null,
        limitation: String? = null,
        category: PrivacyCategory = PrivacyCategory.BLE_TRACKER,
        severity: FindingSeverity = FindingSeverity.NEARBY,
    ): PrivacyFinding {
        val key = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, id)
        return PrivacyFinding(id, key, key.source, id, key, title, evidence, limitation, category, severity,
            Ownership.UNKNOWN, -60, null, null, 1_000, null, true)
    }
}
