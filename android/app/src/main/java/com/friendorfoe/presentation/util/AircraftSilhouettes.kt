package com.friendorfoe.presentation.util

import com.friendorfoe.R
import com.friendorfoe.domain.model.ObjectCategory

enum class SilhouetteCategory {
    NARROWBODY, WIDEBODY, REGIONAL, TURBOPROP, BIZJET,
    HELICOPTER, FIGHTER, CARGO, LIGHTPLANE, DRONE
}

/**
 * Maps an ICAO type designator to a silhouette category.
 * Covers ~190 common type codes.
 */
fun silhouetteForTypeCode(typeCode: String?): SilhouetteCategory? {
    if (typeCode.isNullOrBlank()) return null
    return TYPE_CODE_MAP[typeCode.uppercase()]
}

/**
 * Fallback: maps an ObjectCategory to a silhouette when no type code is available.
 */
fun silhouetteForCategory(category: ObjectCategory): SilhouetteCategory {
    return when (category) {
        ObjectCategory.COMMERCIAL -> SilhouetteCategory.NARROWBODY
        ObjectCategory.GENERAL_AVIATION -> SilhouetteCategory.LIGHTPLANE
        ObjectCategory.MILITARY -> SilhouetteCategory.FIGHTER
        ObjectCategory.HELICOPTER -> SilhouetteCategory.HELICOPTER
        ObjectCategory.GOVERNMENT -> SilhouetteCategory.NARROWBODY
        ObjectCategory.EMERGENCY -> SilhouetteCategory.NARROWBODY
        ObjectCategory.CARGO -> SilhouetteCategory.CARGO
        ObjectCategory.DRONE -> SilhouetteCategory.DRONE
        ObjectCategory.GROUND_VEHICLE -> SilhouetteCategory.LIGHTPLANE
        ObjectCategory.UNKNOWN -> SilhouetteCategory.NARROWBODY
    }
}

/**
 * Returns the drawable resource ID for a silhouette category.
 */
fun silhouetteDrawableRes(silhouette: SilhouetteCategory): Int {
    return when (silhouette) {
        SilhouetteCategory.NARROWBODY -> R.drawable.ic_silhouette_narrowbody
        SilhouetteCategory.WIDEBODY -> R.drawable.ic_silhouette_widebody
        SilhouetteCategory.REGIONAL -> R.drawable.ic_silhouette_regional
        SilhouetteCategory.TURBOPROP -> R.drawable.ic_silhouette_turboprop
        SilhouetteCategory.BIZJET -> R.drawable.ic_silhouette_bizjet
        SilhouetteCategory.HELICOPTER -> R.drawable.ic_silhouette_helicopter
        SilhouetteCategory.FIGHTER -> R.drawable.ic_silhouette_fighter
        SilhouetteCategory.CARGO -> R.drawable.ic_silhouette_cargo
        SilhouetteCategory.LIGHTPLANE -> R.drawable.ic_silhouette_lightplane
        SilhouetteCategory.DRONE -> R.drawable.ic_silhouette_drone
    }
}

private val TYPE_CODE_MAP: Map<String, SilhouetteCategory> = buildMap {
    // Narrowbody
    for (code in listOf(
        "B738", "B737", "B739", "B38M", "B39M",
        "A320", "A321", "A319", "A20N", "A21N",
        "B752", "B753",
        "B712", "A220", "BCS1", "BCS3", "MD80", "B703",
        "B734", "B735", "A318"
    )) put(code, SilhouetteCategory.NARROWBODY)

    // Widebody
    for (code in listOf(
        "B77W", "B772", "B77L", "B788", "B789", "B78X",
        "A332", "A333", "A339", "A359", "A35K", "A388",
        "B744", "B748", "B763", "B764",
        "MD11", "DC10", "A306", "A30B", "A310",
        "A342", "A343", "A345", "A346"
    )) put(code, SilhouetteCategory.WIDEBODY)

    // Regional
    for (code in listOf(
        "CRJ2", "CRJ7", "CRJ9", "CRJX",
        "E170", "E75L", "E75S", "E190", "E195", "E290", "E295",
        "RJ85", "RJ1H", "SU95",
        "E135", "E145", "SF34", "SB20"
    )) put(code, SilhouetteCategory.REGIONAL)

    // Turboprop
    for (code in listOf(
        "AT72", "AT76", "AT43",
        "DH8A", "DH8B", "DH8C", "DH8D",
        "BE20", "BE30", "C208", "PC12", "SW4",
        "TBM7", "TBM8", "TBM9", "PC6", "P46T"
    )) put(code, SilhouetteCategory.TURBOPROP)

    // Bizjet
    for (code in listOf(
        "GLF4", "GLF5", "GLF6", "GLEX",
        "CL35", "CL60", "C56X", "C560", "C680", "C700",
        "LJ35", "LJ45", "LJ60",
        "FA7X", "FA8X", "E55P", "HDJT", "C510",
        "GA5C", "GA6C",
        "C525", "C25A", "C25B",
        "CL30", "G280", "F900", "F2TH", "H25B", "BE40", "PRM1", "E545",
        "SF50", "PC24", "E550", "E545"
    )) put(code, SilhouetteCategory.BIZJET)

    // Helicopter
    for (code in listOf(
        "R44", "R22", "EC35", "EC45", "EC30",
        "AS50", "A109", "A139",
        "B06", "B407", "B429",
        "S76", "S92", "BK17",
        "UH60", "H60", "H47", "CH47",
        "AH64", "UH1",
        "MI8", "MI17", "MI24", "MI26", "MI28", "KA52",
        "CH53", "CH46", "MH60", "SH60", "HH60", "AH1", "AH1Z", "NH90", "EH10", "S70",
        "EC55", "AS32", "B412", "A169", "B505"
    )) put(code, SilhouetteCategory.HELICOPTER)

    // Fighter / military
    for (code in listOf(
        "F16", "F15", "F18", "FA18", "F22", "F35",
        "EUFI", "RFAL",
        "B1", "B2", "B52",
        "T6", "T38", "T45",
        "A10", "C130H", "F117",
        "AV8B", "EA18", "E2C", "E3CF", "E6B",
        "KC10", "KC46", "KC135",
        "V22", "MV22", "CV22", "P8", "P8A", "MQ9",
        "AC130", "U2", "SR71", "C12", "RQ4", "MQ1", "RQ170",
        "SU27", "SU30", "SU34", "SU35", "MIG29", "MIG31",
        "J10", "J20", "JF17",
        "GR4", "GRF4", "MIRA", "MIR2", "KFIR", "F14",
        "F4", "F5",
        "B1B", "B52H", "TU95", "TU160", "TU22"
    )) put(code, SilhouetteCategory.FIGHTER)

    // Cargo
    for (code in listOf(
        "C130", "C17", "C5", "C5M",
        "A400", "A400M", "IL76", "AN124", "C295",
        "C17A", "C30J", "C295W"
    )) put(code, SilhouetteCategory.CARGO)

    // Lightplane
    for (code in listOf(
        "C172", "C182", "C152",
        "P28A", "PA28", "PA32",
        "C210", "C206",
        "BE36", "BE58",
        "DA40", "DA42",
        "SR20", "SR22", "M20P",
        "BE55", "BE76", "PA44", "PA34", "C310", "C340",
        "C414", "C421", "C150", "PA18", "M20T"
    )) put(code, SilhouetteCategory.LIGHTPLANE)
}
