package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory

enum class PrivacyDeviceGroup(val key: String, val label: String, val description: String) {
    TRACKERS("trackers", "Trackers", "Nearby tag and accessory broadcasts"),
    BEACONS("beacons", "Venue beacons", "Routine location broadcasts"),
    EVERYDAY("everyday", "Everyday devices", "Audio, Apple activity and home accessories"),
}

enum class PrivacyDeviceFamily(val key: String, val label: String, val group: PrivacyDeviceGroup) {
    FIND_MY("find_my", "AirTags & Find My", PrivacyDeviceGroup.TRACKERS),
    FIND_HUB("find_hub", "Find Hub", PrivacyDeviceGroup.TRACKERS),
    TILE("tile", "Tile", PrivacyDeviceGroup.TRACKERS),
    SMART_TAG("smart_tag", "Samsung SmartTag", PrivacyDeviceGroup.TRACKERS),
    CHIPOLO("chipolo", "Chipolo", PrivacyDeviceGroup.TRACKERS),
    OTHER_TRACKERS("other_trackers", "Other trackers", PrivacyDeviceGroup.TRACKERS),
    IBEACON("ibeacon", "iBeacon", PrivacyDeviceGroup.BEACONS),
    EDDYSTONE("eddystone", "Eddystone", PrivacyDeviceGroup.BEACONS),
    OTHER_BEACONS("other_beacons", "Other beacons", PrivacyDeviceGroup.BEACONS),
    AIRPODS("airpods", "AirPods activity", PrivacyDeviceGroup.EVERYDAY),
    APPLE_ACTIVITY("apple_activity", "Apple Continuity activity", PrivacyDeviceGroup.EVERYDAY),
    BLUETOOTH_AUDIO("bluetooth_audio", "Headphones & LE Audio", PrivacyDeviceGroup.EVERYDAY),
    INPUT_DEVICES("input_devices", "Bluetooth input devices", PrivacyDeviceGroup.EVERYDAY),
    SMART_HOME("smart_home", "Smart speakers, hubs & TVs", PrivacyDeviceGroup.EVERYDAY),
}

data class PrivacyFamilyBranch(
    val family: PrivacyDeviceFamily,
    val findings: List<PrivacyFinding>,
) {
    val networks: List<PrivacyBeaconNetwork>
        get() = if (family != PrivacyDeviceFamily.IBEACON || findings.none { it.beaconUuid != null }) emptyList()
        else findings.groupBy { it.beaconUuid?.lowercase() }.map { (uuid, rows) ->
            PrivacyBeaconNetwork("ibeacon_uuid:${uuid ?: "unknown"}", uuid, rows)
        }.sortedBy { it.uuid ?: "~" }
}

data class PrivacyBeaconNetwork(val key: String, val uuid: String?, val findings: List<PrivacyFinding>)

data class PrivacyGroupBranch(
    val group: PrivacyDeviceGroup,
    val families: List<PrivacyFamilyBranch>,
) {
    val findings: List<PrivacyFinding> = families.flatMap { it.findings }
}

/** Presentation labels only: never merge identities or change severity/alert policy. */
fun PrivacyFinding.deviceFamily(): PrivacyDeviceFamily? {
    if (category !in TRACKER_AND_BEACON_CATEGORIES) return everydayFamily()
    val words = FAMILY_SEPARATORS.replace(listOfNotNull(title, evidence).joinToString(" ").lowercase(), " ")
    val tokens = words.split(' ').toSet()
    return when (category) {
        PrivacyCategory.VENUE_BEACON -> when {
            "ibeacon" in tokens -> PrivacyDeviceFamily.IBEACON
            "eddystone" in tokens -> PrivacyDeviceFamily.EDDYSTONE
            else -> PrivacyDeviceFamily.OTHER_BEACONS
        }
        PrivacyCategory.FINDMY -> PrivacyDeviceFamily.FIND_MY
        else -> when {
            tokens.any { it in FIND_MY_TOKENS } || " find my " in " $words " -> PrivacyDeviceFamily.FIND_MY
            " find hub " in " $words " || "fmdn" in tokens -> PrivacyDeviceFamily.FIND_HUB
            "tile" in tokens -> PrivacyDeviceFamily.TILE
            "smarttag" in tokens || "samsung" in tokens -> PrivacyDeviceFamily.SMART_TAG
            "chipolo" in tokens -> PrivacyDeviceFamily.CHIPOLO
            else -> PrivacyDeviceFamily.OTHER_TRACKERS
        }
    }
}

private fun PrivacyFinding.everydayFamily(): PrivacyDeviceFamily? {
    if (source == PrivacySourceKind.WIFI_ANALYSIS || wifiNetwork != null) return null
    return when (category) {
        PrivacyCategory.APPLE_CONTINUITY, PrivacyCategory.INFORMATIONAL -> when {
            appleEvidence?.airPodsAssociationEvidence == true || AIRPODS_NAME.containsMatchIn(title) ->
                PrivacyDeviceFamily.AIRPODS
            category == PrivacyCategory.APPLE_CONTINUITY -> PrivacyDeviceFamily.APPLE_ACTIVITY
            HEADPHONE_NAME.containsMatchIn(title) -> PrivacyDeviceFamily.BLUETOOTH_AUDIO
            else -> null
        }
        PrivacyCategory.AURACAST -> PrivacyDeviceFamily.BLUETOOTH_AUDIO
        PrivacyCategory.BLE_HID -> PrivacyDeviceFamily.INPUT_DEVICES
        PrivacyCategory.SMART_SPEAKER, PrivacyCategory.SMART_HOME_HUB, PrivacyCategory.SMART_TV ->
            PrivacyDeviceFamily.SMART_HOME
        else -> null
    }
}

// Keep actionable evidence in the prominent severity sections, while allowing
// family-name searches to find it alongside ordinary broadcasts.
fun PrivacyFinding.deviceTreeFamily(): PrivacyDeviceFamily? =
    if (severity.rank < FindingSeverity.AWARENESS.rank) deviceFamily() else null

fun groupPrivacyDevices(findings: List<PrivacyFinding>): List<PrivacyGroupBranch> {
    val byFamily = findings.mapNotNull { finding -> finding.deviceTreeFamily()?.let { it to finding } }
        .groupBy({ it.first }, { it.second })
    return PrivacyDeviceGroup.entries.mapNotNull { group ->
        val families = PrivacyDeviceFamily.entries.filter { it.group == group }.mapNotNull { family ->
            byFamily[family]?.let { PrivacyFamilyBranch(family, it) }
        }
        families.takeIf { it.isNotEmpty() }?.let { PrivacyGroupBranch(group, it) }
    }
}

private val FAMILY_SEPARATORS = Regex("[^a-z0-9]+")
private val FIND_MY_TOKENS = setOf("airtag", "airtags", "findmy")
private val TRACKER_AND_BEACON_CATEGORIES = setOf(PrivacyCategory.VENUE_BEACON, PrivacyCategory.BLE_TRACKER, PrivacyCategory.FINDMY)
private val AIRPODS_NAME = Regex("""\bAirPods?\b""", RegexOption.IGNORE_CASE)
private val HEADPHONE_NAME = Regex("""\b(headphones?|earbuds?|Galaxy Buds|Pixel Buds)\b""", RegexOption.IGNORE_CASE)
