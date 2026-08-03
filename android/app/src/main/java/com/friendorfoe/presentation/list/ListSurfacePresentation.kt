package com.friendorfoe.presentation.list

import androidx.compose.ui.graphics.Color
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.permissions.PermissionSettingsLaunchResult
import com.friendorfoe.presentation.permissions.PermissionUiState

sealed interface ListBodyState {
    data object Loading : ListBodyState
    data class Results(val rows: List<SkyObject>) : ListBodyState
    data class StaleResults(
        val rows: List<SkyObject>,
        val ageMs: Long?,
        val message: String,
    ) : ListBodyState
    data object NoDetections : ListBodyState
    data class NoMatches(val activeFilterCount: Int) : ListBodyState
    data class Failed(val message: String) : ListBodyState
}

data class ListUiState(
    val filter: FilterState = FilterState(),
    val activeFilterCount: Int = 0,
    val body: ListBodyState = ListBodyState.Loading,
    val locationPermissionState: PermissionUiState = PermissionUiState.Loading,
    val locationSettingsLaunchFailed: Boolean = false,
)

data class ListActions(
    val onQueryChanged: (String) -> Unit = {},
    val onOpenFilters: () -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onOpenPeek: (SkyObject) -> Unit = {},
    val onRequestLocation: () -> Unit = {},
    val onOpenLocationSettings: () -> PermissionSettingsLaunchResult = {
        PermissionSettingsLaunchResult.Failed
    },
)

fun reduceListBody(
    raw: List<SkyObject>,
    visible: List<SkyObject>,
    resolved: Boolean,
    failure: String?,
    cacheAgeMs: Long? = null,
    activeFilterCount: Int = 0,
): ListBodyState = when {
    !resolved -> ListBodyState.Loading
    failure != null && visible.isNotEmpty() -> ListBodyState.StaleResults(
        rows = visible,
        ageMs = cacheAgeMs,
        message = failure,
    )
    failure != null -> ListBodyState.Failed(failure)
    raw.isEmpty() -> ListBodyState.NoDetections
    visible.isEmpty() -> ListBodyState.NoMatches(activeFilterCount)
    else -> ListBodyState.Results(visible)
}

internal data class ListBadgeVisual(
    val label: String,
    val color: Color
)

internal fun listPrimaryText(skyObject: SkyObject): String = when (skyObject) {
    is Aircraft -> {
        val publicSafetyLabel = publicSafetyAircraftLabel(skyObject)
        val identifier = skyObject.callsign?.trim()?.takeIf { it.isNotBlank() } ?: skyObject.icaoHex
        if (publicSafetyLabel != null) {
            "$publicSafetyLabel  $identifier"
        } else if (skyObject.aircraftType != null) {
            "$identifier  ${skyObject.aircraftType}"
        } else {
            identifier
        }
    }
    is Drone -> skyObject.droneId
}

internal fun listSecondaryText(skyObject: SkyObject): String = when (skyObject) {
    is Aircraft -> {
        if (isPublicSafetyAircraft(skyObject)) {
            listOfNotNull(
                skyObject.operatorName?.trim()?.takeIf { it.isNotBlank() },
                skyObject.aircraftModel ?: skyObject.aircraftType,
                skyObject.registration?.trim()?.takeIf { it.isNotBlank() }
            ).joinToString(" - ").ifBlank {
                skyObject.aircraftModel ?: skyObject.aircraftType ?: "Unknown aircraft"
            }
        } else {
            skyObject.aircraftModel ?: skyObject.aircraftType ?: "Unknown aircraft"
        }
    }
    is Drone -> {
        val parts = listOfNotNull(skyObject.manufacturer, skyObject.model)
        if (parts.isNotEmpty()) parts.joinToString(" ") else "Unknown drone"
    }
}

internal fun listBadgeText(skyObject: SkyObject): String? = listBadgeVisual(skyObject)?.label

internal fun listBadgeVisual(skyObject: SkyObject): ListBadgeVisual? = when (skyObject) {
    is Aircraft -> publicSafetyBadgeText(skyObject)?.let { badge ->
        ListBadgeVisual(label = badge, color = publicSafetyBadgeColor(badge))
    }
    else -> null
}

internal fun listAttentionColor(skyObject: SkyObject): Color? =
    listBadgeVisual(skyObject)?.color ?: when (skyObject.category) {
        ObjectCategory.MILITARY -> Color(0xFFF44336)
        ObjectCategory.GOVERNMENT -> Color(0xFFE65100)
        ObjectCategory.EMERGENCY -> Color(0xFFE91E63)
        else -> null
    }

internal fun listSourceLabel(source: DetectionSource): String = when (source) {
    DetectionSource.ADS_B -> "ADS-B"
    DetectionSource.REMOTE_ID -> "Remote ID"
    DetectionSource.WIFI_NAN, DetectionSource.WIFI_BEACON -> "Remote ID · Wi-Fi"
    DetectionSource.WIFI -> "Phone"
}

internal fun listCategoryLabel(category: ObjectCategory): String = when (category) {
    ObjectCategory.COMMERCIAL -> "Commercial"
    ObjectCategory.GENERAL_AVIATION -> "General aviation"
    ObjectCategory.MILITARY -> "Military"
    ObjectCategory.HELICOPTER -> "Helicopter"
    ObjectCategory.GOVERNMENT -> "Government"
    ObjectCategory.EMERGENCY -> "Emergency"
    ObjectCategory.CARGO -> "Cargo"
    ObjectCategory.DRONE -> "Drone"
    ObjectCategory.GROUND_VEHICLE -> "Ground vehicle"
    ObjectCategory.UNKNOWN -> "Unknown"
}

internal fun listAttentionLabel(skyObject: SkyObject): String? = when (listBadgeText(skyObject)) {
    "LAW" -> "Law enforcement"
    "FIRE" -> "Fire / rescue"
    "EMS" -> "Emergency medical"
    "PS" -> "Public safety"
    else -> when (skyObject.category) {
        ObjectCategory.MILITARY -> "Military"
        ObjectCategory.GOVERNMENT -> "Government"
        ObjectCategory.EMERGENCY -> "Emergency"
        else -> null
    }
}

internal fun listSurfacePriority(skyObject: SkyObject): Int = when (skyObject) {
    is Aircraft -> when {
        isPublicSafetyAircraft(skyObject) && isRotorcraft(skyObject) -> 50
        isPublicSafetyAircraft(skyObject) -> 45
        skyObject.category == ObjectCategory.EMERGENCY -> 40
        skyObject.category == ObjectCategory.MILITARY -> 35
        skyObject.category == ObjectCategory.GOVERNMENT -> 30
        skyObject.category == ObjectCategory.HELICOPTER -> 20
        else -> 0
    }
    is Drone -> 25
}

private fun publicSafetyBadgeColor(badge: String): Color = when (badge) {
    "LAW" -> Color(0xFFE65100)
    "FIRE" -> Color(0xFFD32F2F)
    "EMS" -> Color(0xFFE91E63)
    else -> Color(0xFF1565C0)
}

private fun publicSafetyAircraftLabel(aircraft: Aircraft): String? {
    if (!isPublicSafetyAircraft(aircraft)) return null
    val agency = when {
        evidenceContains(aircraft, "SHERIFF") -> "SHERIFF"
        evidenceContains(aircraft, "POLICE") -> "POLICE"
        evidenceContains(aircraft, "HIGHWAY PATROL") -> "POLICE"
        evidenceContains(aircraft, "STATE TROOPER") -> "POLICE"
        evidenceContains(aircraft, "FIRE") -> "FIRE"
        evidenceContains(aircraft, "CALFIRE") -> "FIRE"
        evidenceContains(aircraft, "MEDEVAC") -> "EMS"
        evidenceContains(aircraft, "MEDICAL") -> "EMS"
        evidenceContains(aircraft, "RESCUE") -> "RESCUE"
        else -> "PUBLIC SAFETY"
    }
    return if (isRotorcraft(aircraft)) "$agency HELICOPTER" else "$agency AIRCRAFT"
}

private fun publicSafetyBadgeText(aircraft: Aircraft): String? {
    if (!isPublicSafetyAircraft(aircraft)) return null
    return when {
        evidenceContains(aircraft, "SHERIFF") -> "LAW"
        evidenceContains(aircraft, "POLICE") -> "LAW"
        evidenceContains(aircraft, "HIGHWAY PATROL") -> "LAW"
        evidenceContains(aircraft, "STATE TROOPER") -> "LAW"
        evidenceContains(aircraft, "FIRE") -> "FIRE"
        evidenceContains(aircraft, "CALFIRE") -> "FIRE"
        evidenceContains(aircraft, "MEDEVAC") -> "EMS"
        evidenceContains(aircraft, "MEDICAL") -> "EMS"
        else -> "PS"
    }
}

private fun isPublicSafetyAircraft(aircraft: Aircraft): Boolean {
    if (aircraft.category == ObjectCategory.GOVERNMENT || aircraft.category == ObjectCategory.EMERGENCY) {
        if (hasPublicSafetyEvidence(aircraft)) return true
    }
    return hasPublicSafetySignal(aircraft)
}

private fun hasPublicSafetyEvidence(aircraft: Aircraft): Boolean =
    listOfNotNull(aircraft.operatorName, aircraft.callsign).any { value ->
        val normalized = value.uppercase()
        PUBLIC_SAFETY_TERMS.any { normalized.contains(it) }
    }

private fun hasPublicSafetySignal(aircraft: Aircraft): Boolean =
    aircraft.classificationSignals.orEmpty().any { signal ->
        signal == "OWNER:PUBLIC_SAFETY" ||
            signal == "CALLSIGN:PUBLIC_SAFETY" ||
            signal == "CALLSIGN:LAW_ENFORCEMENT"
    }

private fun evidenceContains(aircraft: Aircraft, needle: String): Boolean =
    listOfNotNull(aircraft.operatorName, aircraft.callsign).any { it.uppercase().contains(needle) }

private fun isRotorcraft(aircraft: Aircraft): Boolean {
    if (aircraft.category == ObjectCategory.HELICOPTER) return true
    val typeCode = aircraft.aircraftType?.trim()?.uppercase()
    if (typeCode != null && typeCode in ROTORCRAFT_TYPE_CODES) return true
    val model = aircraft.aircraftModel?.uppercase() ?: return false
    return ROTORCRAFT_MODEL_TERMS.any { model.contains(it) }
}

private val PUBLIC_SAFETY_TERMS = listOf(
    "SHERIFF",
    "POLICE",
    "HIGHWAY PATROL",
    "STATE TROOPER",
    "PUBLIC SAFETY",
    "FIRE",
    "CALFIRE",
    "MEDEVAC",
    "MEDICAL",
    "RESCUE"
)

private val ROTORCRAFT_TYPE_CODES = setOf(
    "A109", "A119", "A139", "A169", "A189",
    "AS32", "AS50", "AS55", "AS65",
    "B06", "B105", "B212", "B222", "B407", "B412", "B429",
    "BK17",
    "EC20", "EC30", "EC35", "EC45", "EC55",
    "H125", "H130", "H135", "H145",
    "H500", "MD50", "MD52", "MD60", "MD90",
    "R22", "R44", "R66",
    "S61", "S76", "S92",
    "H60", "UH60", "HH60", "MH60", "SH60",
    "H47", "CH47", "CH53", "CH46",
    "AH64", "AH1", "AH1Z"
)

private val ROTORCRAFT_MODEL_TERMS = listOf(
    "HELICOPTER",
    "ROTOR",
    "EUROCOPTER",
    "AIRBUS HELICOPTERS",
    "AS350",
    "H125",
    "H130",
    "H135",
    "H145",
    "BELL 206",
    "BELL 407",
    "BELL 412",
    "BELL 429",
    "ROBINSON R44",
    "ROBINSON R66",
    "SIKORSKY",
    "BLACK HAWK"
)
