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
    "B712", // Boeing 717
    "A220", "BCS1", "BCS3", // Airbus A220 family
    "MD80", // McDonnell Douglas MD-80
    // Widebody
    "B77W", "B772", "B77L", "B788", "B789", "B78X",
    "A332", "A333", "A339", "A359", "A35K", "A388",
    "B744", "B748", "B763", "B764",
    "MD11", "DC10", // McDonnell Douglas widebody
    "A306", "A30B", "A310", // Airbus classics
    "B703", // Boeing 707
    "SU95", // Sukhoi Superjet
    "RJ85", "RJ1H", // Avro RJ / BAe 146
    // Regional
    "CRJ2", "CRJ7", "CRJ9", "CRJX",
    "E170", "E75L", "E75S", "E190", "E195", "E290", "E295",
    // Turboprop
    "AT72", "AT76", "AT43",
    "DH8A", "DH8B", "DH8C", "DH8D",
    "BE20", "BE30", "C208", "PC12", "SW4",
    "TBM7", "TBM8", "TBM9", // Daher TBM family
    "PC6", // Pilatus Porter
    // Bizjet
    "GLF4", "GLF5", "GLF6", "GLEX",
    "CL35", "CL60", "C56X", "C560", "C680", "C700",
    "LJ35", "LJ45", "LJ60",
    "FA7X", "FA8X", "E55P", "HDJT", "C510",
    "GA5C", "GA6C",
    "C525", "C25A", "C25B", // Citation CJ variants
    // Helicopter
    "R44", "R22", "EC35", "EC45", "EC30",
    "AS50", "A109", "A139",
    "B06", "B407", "B429",
    "S76", "S92", "BK17",
    "UH60", "H60", "CH47", "CH53", // Military helicopters
    "MI8", "MI24", "MI28", "KA52", // Russian helicopters
    "V22", // V-22 Osprey
    // Fighter / military
    "F14", "F16", "F15", "F18", "FA18", "F22", "F35",
    "EUFI", "RFAL", "GRF4", "GR4", "MIR2", "KFIR", // NATO/allied fighters
    "SU27", "SU30", "SU34", "SU35", "MIG29", "MIG31", // Russian fighters
    "J10", "J20", "JF17", // Chinese/Pakistani fighters
    "TU95", "TU160", "TU22", // Russian bombers
    "B1", "B2", "B52",
    "T6", "T38", "T45",
    "A10", "C130H", "F117",
    "AV8B", "EA18", "E2C", "E3CF", "E6B",
    "KC10", "KC46", "KC135",
    "P8", "P8A", // P-8 Poseidon
    "MQ9", // MQ-9 Reaper
    // Cargo
    "C130", "C17", "C17A", "C5", "C5M",
    "A400", "A400M", "IL76", "AN124", "C295",
    "C30J", "C295W", // C-130J / C-295 variants
    // Lightplane
    "C172", "C182", "C152",
    "P28A", "PA28", "PA32",
    "C210", "C206",
    "BE36", "BE58",
    "DA40", "DA42",
    "SR20", "SR22", "M20P",
    "P46T", // Piper Malibu/Meridian
    // GA twins & singles
    "BE55", "BE76", "PA44", "PA34", "C310", "C340",
    "C414", "C421", "C150", "PA18", "M20T",
    // Bizjets
    "CL30", "G280", "F900", "F2TH", "H25B", "BE40", "PRM1", "E545",
    // Military
    "AC130", "U2", "SR71", "AH64", "UH1", "C12",
    // Regional jets
    "E135", "E145", "SF34", "SB20",
    // Airliners
    "B734", "B735", "A318", "A342", "A343", "A345", "A346"
)

/**
 * Returns a file:///android_asset/ URI for an aircraft type photo,
 * or null if no photo is available for that type.
 *
 * Delegates to AircraftDatabase as the single source of truth for variant-to-photo
 * mapping. For example, B737 will use B738.jpg (the NG representative) rather than
 * a potentially incorrect B737.jpg.
 */
fun getAircraftPhotoUrl(typeCode: String?): String? {
    if (typeCode.isNullOrBlank()) return null
    val normalized = typeCode.trim().uppercase()
    // Use AircraftDatabase as single source of truth for photo mapping
    val dbEntry = AircraftDatabase.matchByTypeCode(normalized)
    // Extract just the filename without path/extension from photoAsset (e.g., "aircraft/B738.jpg" → "B738")
    val rawPhotoCode = dbEntry?.photoAsset
        ?.substringAfterLast("/")
        ?.substringBeforeLast(".")
    val photoCode = if (!rawPhotoCode.isNullOrBlank()) rawPhotoCode else normalized

    if (photoCode in AVAILABLE_PHOTOS) {
        return "file:///android_asset/aircraft/$photoCode.jpg"
    }
    // Fuzzy match: try stripping trailing variant letter (B39M → B39, then B738 family)
    val stripped = photoCode.replace(Regex("[A-Z]$"), "")
    if (stripped != photoCode && stripped in AVAILABLE_PHOTOS) {
        return "file:///android_asset/aircraft/$stripped.jpg"
    }
    // Try common prefix (first 3 chars) — e.g., MH60 matches UH60 family
    val prefix = photoCode.take(3)
    val prefixMatch = AVAILABLE_PHOTOS.firstOrNull { it.startsWith(prefix) }
    if (prefixMatch != null) {
        return "file:///android_asset/aircraft/$prefixMatch.jpg"
    }
    // Try without leading letter for military variants (CH53 → H53 → no, but SH60 → H60 → UH60)
    if (photoCode.length >= 3) {
        val noPrefix = photoCode.drop(1)
        if (noPrefix in AVAILABLE_PHOTOS) {
            return "file:///android_asset/aircraft/$noPrefix.jpg"
        }
        // Try known military helo prefix swaps: MH60/SH60/HH60 → UH60
        val heloBase = photoCode.replace(Regex("^[MSH]H"), "UH")
        if (heloBase != photoCode && heloBase in AVAILABLE_PHOTOS) {
            return "file:///android_asset/aircraft/$heloBase.jpg"
        }
    }
    return null
}
