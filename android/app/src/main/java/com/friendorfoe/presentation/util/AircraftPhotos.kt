package com.friendorfoe.presentation.util

/**
 * Set of ICAO type codes that have photos bundled as assets.
 * Matches the files in assets/aircraft/{TYPE}.jpg
 */
private val AVAILABLE_PHOTOS: Set<String> = setOf(
    // Narrowbody
    "B738", "B737", "B739", "B38M", "B39M",
    "A320", "A321", "A319", "A20N", "A21N",
    "B752", "B753",
    // Widebody
    "B77W", "B772", "B77L", "B788", "B789", "B78X",
    "A332", "A333", "A339", "A359", "A35K", "A388",
    "B744", "B748", "B763", "B764",
    // Regional
    "CRJ2", "CRJ7", "CRJ9", "CRJX",
    "E170", "E75L", "E75S", "E190", "E195", "E290", "E295",
    // Turboprop
    "AT72", "AT76", "AT43",
    "DH8A", "DH8B", "DH8C", "DH8D",
    "BE20", "BE30", "C208", "PC12", "SW4",
    // Bizjet
    "GLF4", "GLF5", "GLF6", "GLEX",
    "CL35", "CL60", "C56X", "C560", "C680", "C700",
    "LJ35", "LJ45", "LJ60",
    "FA7X", "FA8X", "E55P", "HDJT", "C510",
    "GA5C", "GA6C",
    // Helicopter
    "R44", "R22", "EC35", "EC45", "EC30",
    "AS50", "A109", "A139",
    "B06", "B407", "B429",
    "S76", "S92", "BK17",
    // Fighter / military
    "F16", "F15", "F18", "FA18", "F22", "F35",
    "EUFI", "RFAL",
    "B1", "B2", "B52",
    "T6", "T38", "T45",
    "A10", "C130H", "F117",
    "AV8B", "EA18", "E2C", "E3CF", "E6B",
    "KC10", "KC46", "KC135",
    // Cargo
    "C130", "C17", "C5", "C5M",
    "A400", "A400M", "IL76", "AN124", "C295",
    // Lightplane
    "C172", "C182", "C152",
    "P28A", "PA28", "PA32",
    "C210", "C206",
    "BE36", "BE58",
    "DA40", "DA42",
    "SR20", "SR22", "M20P"
)

/**
 * Returns a file:///android_asset/ URI for an aircraft type photo,
 * or null if no photo is available for that type.
 * Coil loads these URIs natively from bundled APK assets.
 */
fun getAircraftPhotoUrl(typeCode: String?): String? {
    if (typeCode.isNullOrBlank()) return null
    val normalized = typeCode.uppercase()
    return if (normalized in AVAILABLE_PHOTOS) {
        "file:///android_asset/aircraft/$normalized.jpg"
    } else {
        null
    }
}
