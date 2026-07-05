package com.friendorfoe.detection

import com.friendorfoe.domain.model.ObjectCategory

/**
 * Multi-signal scoring classifier for military and government aircraft.
 *
 * Uses live ADS-B and metadata signals:
 * 1. ICAO hex address ranges (known military allocations)
 * 2. Callsign patterns (military/government call prefixes)
 * 3. ICAO type designator codes (military aircraft types)
 * 4. readsb database flags and owner/operator names when feeds provide them
 *
 * Scoring: each signal contributes a score. If the total meets the threshold,
 * the aircraft is classified as MILITARY or GOVERNMENT.
 */
object MilitaryClassifier {

    private const val THRESHOLD = 40

    data class ClassificationResult(
        val category: ObjectCategory?,
        val score: Int,
        val signals: List<String>
    )

    /**
     * Classify an aircraft using all available signals.
     * Returns null category if score is below threshold.
     */
    fun classify(
        icaoHex: String?,
        callsign: String?,
        typeCode: String?,
        registration: String?,
        dbFlags: Int? = null,
        ownerName: String? = null,
        operatorName: String? = null
    ): ClassificationResult {
        val signals = mutableListOf<String>()
        var score = 0
        var isGovernment = false

        // Signal 1: readsb/tar1090 database flags. Bit 0 marks military aircraft
        // when the upstream receiver has a database file loaded.
        if (dbFlags != null && dbFlags and 1 == 1) {
            score += 45
            signals.add("DBFLAGS:MILITARY")
        }

        // Signal 2: public aircraft registration patterns
        if (registration != null) {
            val regResult = checkRegistration(registration.uppercase().trim())
            if (regResult != null) {
                score += 45
                signals.add("REG:${regResult.tag}")
                if (regResult.isGovernment) isGovernment = true
            }
        }

        // Signal 3: owner/operator names from aircraft databases, when present.
        for (name in listOfNotNull(ownerName, operatorName)) {
            val ownerResult = checkOwner(name.uppercase().trim())
            if (ownerResult != null) {
                score += 45
                signals.add("OWNER:${ownerResult.tag}")
                if (ownerResult.isGovernment) isGovernment = true
                break
            }
        }

        // Signal 4: ICAO hex range
        if (icaoHex != null) {
            val hexResult = checkIcaoHex(icaoHex.lowercase())
            if (hexResult != null) {
                score += 40
                signals.add("ICAO:${hexResult.tag}")
                if (hexResult.isGovernment) isGovernment = true
            }
        }

        // Signal 5: Callsign pattern
        if (callsign != null) {
            val csResult = checkCallsign(callsign.uppercase().trim())
            if (csResult != null) {
                score += csResult.score
                signals.add("CALLSIGN:${csResult.tag}")
                if (csResult.isGovernment) isGovernment = true
            }
        }

        // Signal 6: Type code
        if (typeCode != null) {
            val typeResult = checkTypeCode(typeCode.uppercase().trim())
            if (typeResult != null) {
                score += typeResult.score
                signals.add("TYPE:${typeResult.tag}")
            }
        }

        if (score < THRESHOLD) {
            return ClassificationResult(category = null, score = score, signals = signals)
        }

        val category = if (isGovernment) ObjectCategory.GOVERNMENT else ObjectCategory.MILITARY
        return ClassificationResult(category = category, score = score, signals = signals)
    }

    // ---- Signal 1: ICAO Hex Ranges ----

    private data class HexRangeEntry(
        val start: Long,
        val end: Long,
        val tag: String,
        val isGovernment: Boolean = false
    )

    private val icaoHexRanges: List<HexRangeEntry> by lazy {
        listOf(
            // USA — DoD
            HexRangeEntry(0xAE0000, 0xAEFFFF, "US_MIL_DOD"),
            HexRangeEntry(0xAF0000, 0xAFFFFF, "US_MIL_DOD2"),
            // USA — Coast Guard
            HexRangeEntry(0xAD0000, 0xAD0FFF, "US_COAST_GUARD", isGovernment = true),
            // USA — CBP
            HexRangeEntry(0xAC0000, 0xAC0FFF, "US_CBP", isGovernment = true),
            // UK — RAF/MoD
            HexRangeEntry(0x43C000, 0x43FFFF, "UK_RAF"),
            // France — Armee de l'Air
            HexRangeEntry(0x3A0000, 0x3A0FFF, "FR_MIL"),
            HexRangeEntry(0x3B0000, 0x3BFFFF, "FR_MIL2"),
            // Germany — Luftwaffe
            HexRangeEntry(0x3F0000, 0x3F0FFF, "DE_MIL"),
            HexRangeEntry(0x3F4000, 0x3F4FFF, "DE_MIL2"),
            // Australia — RAAF
            HexRangeEntry(0x7C8000, 0x7C8FFF, "AU_MIL"),
            // Canada — RCAF
            HexRangeEntry(0xC20000, 0xC20FFF, "CA_MIL"),
            // Italy — AMI
            HexRangeEntry(0x300000, 0x300FFF, "IT_MIL"),
            // Spain — Ejercito del Aire
            HexRangeEntry(0x340000, 0x340FFF, "ES_MIL"),
            // Netherlands — RNLAF
            HexRangeEntry(0x480000, 0x480FFF, "NL_MIL"),
            // Belgium — BAF
            HexRangeEntry(0x440000, 0x440FFF, "BE_MIL"),
            // Turkey — TAF
            HexRangeEntry(0x4B0000, 0x4B0FFF, "TR_MIL"),
            // Sweden — SwAF
            HexRangeEntry(0x4A0000, 0x4A0FFF, "SE_MIL"),
            // Norway — RNoAF
            HexRangeEntry(0x478000, 0x478FFF, "NO_MIL"),
            // Denmark — RDAF
            HexRangeEntry(0x458000, 0x458FFF, "DK_MIL"),
            // Poland — PAF
            HexRangeEntry(0x488000, 0x488FFF, "PL_MIL"),
            // Greece — HAF
            HexRangeEntry(0x468000, 0x468FFF, "GR_MIL"),
            // Israel — IAF
            HexRangeEntry(0x738000, 0x738FFF, "IL_MIL"),
            // Japan — JASDF
            HexRangeEntry(0x840000, 0x840FFF, "JP_MIL"),
            // South Korea — ROKAF
            HexRangeEntry(0x71C000, 0x71FFFF, "KR_MIL"),
            // Russia
            HexRangeEntry(0x150000, 0x150FFF, "RU_MIL"),
            // China
            HexRangeEntry(0x780000, 0x780FFF, "CN_MIL"),
            // India
            HexRangeEntry(0x800000, 0x800FFF, "IN_MIL"),
            // NATO
            HexRangeEntry(0x0F0000, 0x0F0FFF, "NATO"),
            // Brazil
            HexRangeEntry(0xE40000, 0xE40FFF, "BR_MIL"),
            // Switzerland
            HexRangeEntry(0x4B8000, 0x4B8FFF, "CH_MIL"),
            // Singapore
            HexRangeEntry(0x760000, 0x760FFF, "SG_MIL"),
            // UAE
            HexRangeEntry(0x890000, 0x890FFF, "AE_MIL"),
            // Saudi Arabia
            HexRangeEntry(0x710800, 0x710FFF, "SA_MIL")
        )
    }

    private data class HexResult(val tag: String, val isGovernment: Boolean)

    private fun checkIcaoHex(hex: String): HexResult? {
        val value = hex.toLongOrNull(16) ?: return null
        for (range in icaoHexRanges) {
            if (value in range.start..range.end) {
                return HexResult(tag = range.tag, isGovernment = range.isGovernment)
            }
        }
        return null
    }

    // ---- Signal 2: Callsign Patterns ----

    private data class CallsignPattern(
        val regex: Regex,
        val tag: String,
        val isGovernment: Boolean = false,
        val score: Int = 35
    )

    private val callsignPatterns: List<CallsignPattern> by lazy {
        listOf(
            // US Military
            CallsignPattern(Regex("^RCH\\d+"), "REACH_AMC", score = 45),
            CallsignPattern(Regex("^EVAC\\d*"), "EVAC", score = 45),
            CallsignPattern(Regex("^DUKE\\d+"), "DUKE"),
            CallsignPattern(Regex("^NAVY\\d*"), "NAVY", score = 45),
            CallsignPattern(Regex("^ARMY\\d*"), "ARMY", score = 45),
            CallsignPattern(Regex("^TOPCAT\\d*"), "TOPCAT"),
            CallsignPattern(Regex("^TEAL\\d+"), "TEAL", score = 45),
            CallsignPattern(Regex("^ORDER\\d+"), "ORDER", score = 45),
            CallsignPattern(Regex("^MOOSE\\d+"), "MOOSE"),
            CallsignPattern(Regex("^PACK\\d+"), "PACK"),
            CallsignPattern(Regex("^RAGE\\d+"), "RAGE"),
            CallsignPattern(Regex("^HAWK\\d+"), "HAWK"),
            CallsignPattern(Regex("^VIPER\\d+"), "VIPER"),
            CallsignPattern(Regex("^COBRA\\d+"), "COBRA"),
            CallsignPattern(Regex("^SKULL\\d+"), "SKULL"),
            CallsignPattern(Regex("^KNIFE\\d+"), "KNIFE"),
            CallsignPattern(Regex("^TABOO\\d+"), "TABOO"),
            CallsignPattern(Regex("^SNTRY\\d*"), "SENTRY", score = 45),
            CallsignPattern(Regex("^GUCCI\\d+"), "GUCCI"),
            CallsignPattern(Regex("^ROCKY\\d+"), "ROCKY"),
            CallsignPattern(Regex("^STONE\\d+"), "STONE"),
            // Generic US military prefixes
            CallsignPattern(Regex("^AF[0-9]"), "USAF", score = 45),
            CallsignPattern(Regex("^MC[0-9]"), "USMC", score = 45),
            // International military
            CallsignPattern(Regex("^BAF\\d+"), "BELGIAN_AF", score = 45),
            CallsignPattern(Regex("^GAF\\d+"), "GERMAN_AF", score = 45),
            CallsignPattern(Regex("^FAF\\d+"), "FRENCH_AF", score = 45),
            CallsignPattern(Regex("^RAF\\d+"), "ROYAL_AF", score = 45),
            CallsignPattern(Regex("^IAF\\d+"), "ISRAELI_AF", score = 45),
            CallsignPattern(Regex("^IAM\\d+"), "ITALIAN_AF", score = 45),
            CallsignPattern(Regex("^SUI\\d+"), "SWISS_AF", score = 45),
            CallsignPattern(Regex("^NOR\\d+"), "NORWEGIAN_AF", score = 45),
            CallsignPattern(Regex("^DAF\\d+"), "DANISH_AF", score = 45),
            CallsignPattern(Regex("^PLF\\d+"), "POLISH_AF", score = 45),
            CallsignPattern(Regex("^HAF\\d+"), "GREEK_AF", score = 45),
            CallsignPattern(Regex("^TKF\\d+"), "TURKISH_AF", score = 45),
            CallsignPattern(Regex("^SWF\\d+"), "SWEDISH_AF", score = 45),
            CallsignPattern(Regex("^RNF\\d+"), "DUTCH_AF", score = 45),
            CallsignPattern(Regex("^CNF\\d+"), "CANADIAN_AF", score = 45),
            CallsignPattern(Regex("^ASF\\d+"), "AUSTRALIAN_AF", score = 45),
            CallsignPattern(Regex("^RRR\\d+"), "RAF_ASCOT", score = 45),
            CallsignPattern(Regex("^CFC\\d+"), "CANFORCE", score = 45),
            CallsignPattern(Regex("^ASY\\d+"), "AUSSIE_AF", score = 45),
            // Government / law enforcement
            CallsignPattern(Regex("^EXEC\\d*"), "EXECUTIVE", isGovernment = true, score = 45),
            CallsignPattern(Regex("^SAMP\\d+"), "SAM_PRIORITY", isGovernment = true, score = 45),
            CallsignPattern(Regex("^COAST\\d+"), "COAST_GUARD", isGovernment = true, score = 45),
            CallsignPattern(Regex("^CBP\\d+"), "CBP", isGovernment = true, score = 45),
            CallsignPattern(Regex("^PAT\\d+"), "BORDER_PATROL", isGovernment = true, score = 45),
            CallsignPattern(Regex("^USBP\\d*"), "BORDER_PATROL", isGovernment = true, score = 45),
            CallsignPattern(Regex("^COPTER\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^POLICE\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^SHERIFF\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^PATROL\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^DPS\\d+"), "PUBLIC_SAFETY", isGovernment = true, score = 45),
            CallsignPattern(Regex("^CALFIRE\\d*"), "PUBLIC_SAFETY", isGovernment = true, score = 45),
            CallsignPattern(Regex("^FIRE\\d+"), "PUBLIC_SAFETY", isGovernment = true, score = 45),
            CallsignPattern(Regex("^RESCUE\\d+"), "PUBLIC_SAFETY", isGovernment = true, score = 45),
            CallsignPattern(Regex("^TROOPER\\d*"), "STATE_POLICE", isGovernment = true, score = 45),
            CallsignPattern(Regex("^CHP\\d*"), "HIGHWAY_PATROL", isGovernment = true, score = 45),
            CallsignPattern(Regex("^NYPD\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^LAPD\\d*"), "LAW_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^FBI\\d*"), "FBI", isGovernment = true, score = 45),
            CallsignPattern(Regex("^DEA\\d*"), "DEA", isGovernment = true, score = 45),
            CallsignPattern(Regex("^DHS\\d*"), "DHS", isGovernment = true, score = 45),
            CallsignPattern(Regex("^OMAHA\\d*"), "SECRET_SERVICE", isGovernment = true, score = 45),
            CallsignPattern(Regex("^SWORD\\d+"), "US_MARSHALS", isGovernment = true, score = 45),
            CallsignPattern(Regex("^TIGER\\d+"), "DEA", isGovernment = true, score = 45),
            CallsignPattern(Regex("^ICE\\d+"), "ICE", isGovernment = true, score = 45),
            CallsignPattern(Regex("^NARC\\d+"), "DRUG_ENFORCEMENT", isGovernment = true, score = 45),
            CallsignPattern(Regex("^FED\\d+"), "FEDERAL", isGovernment = true, score = 45),
            CallsignPattern(Regex("^NIGHTWATCH"), "NAOC", isGovernment = true, score = 45),
            CallsignPattern(Regex("^ANGEL\\d+"), "PRESIDENTIAL_SUPPORT", isGovernment = true, score = 45),
            // Emergency / medevac
            CallsignPattern(Regex("^LIFEGUARD"), "LIFEGUARD"),
            CallsignPattern(Regex("^MEDEVAC"), "MEDEVAC"),
        )
    }

    private data class CallsignResult(val tag: String, val isGovernment: Boolean, val score: Int)

    private fun checkCallsign(callsign: String): CallsignResult? {
        for (pattern in callsignPatterns) {
            if (pattern.regex.containsMatchIn(callsign)) {
                return CallsignResult(
                    tag = pattern.tag,
                    isGovernment = pattern.isGovernment,
                    score = pattern.score
                )
            }
        }
        return null
    }

    // ---- Public registration and ownership signals ----

    private data class RegistrationResult(val tag: String, val isGovernment: Boolean)

    private fun checkRegistration(registration: String): RegistrationResult? {
        return if (Regex("^N[1-9][0-9]?$").matches(registration)) {
            RegistrationResult("FAA_INTERNAL", isGovernment = true)
        } else {
            null
        }
    }

    private data class OwnerPattern(
        val regex: Regex,
        val tag: String,
        val isGovernment: Boolean
    )

    private val ownerPatterns: List<OwnerPattern> by lazy {
        listOf(
            OwnerPattern(Regex("\\b(USAF|UNITED STATES AIR FORCE|US AIR FORCE)\\b"), "USAF", false),
            OwnerPattern(Regex("\\b(US ARMY|UNITED STATES ARMY|DEPARTMENT OF THE ARMY)\\b"), "US_ARMY", false),
            OwnerPattern(Regex("\\b(US NAVY|UNITED STATES NAVY|DEPARTMENT OF THE NAVY)\\b"), "US_NAVY", false),
            OwnerPattern(Regex("\\b(US MARINE CORPS|UNITED STATES MARINE CORPS)\\b"), "USMC", false),
            OwnerPattern(Regex("\\b(NATIONAL GUARD|AIR NATIONAL GUARD)\\b"), "NATIONAL_GUARD", false),
            OwnerPattern(Regex("\\b(POLICE|SHERIFF|HIGHWAY PATROL|STATE TROOPER|PUBLIC SAFETY)\\b"), "PUBLIC_SAFETY", true),
            OwnerPattern(Regex("\\b(DEPT OF PUBLIC SAFETY|DEPARTMENT OF PUBLIC SAFETY)\\b"), "PUBLIC_SAFETY", true),
            OwnerPattern(Regex("\\b(BORDER PATROL|CUSTOMS|CBP|HOMELAND SECURITY|DHS)\\b"), "FEDERAL_ENFORCEMENT", true),
            OwnerPattern(Regex("\\b(FBI|DEA|US MARSHAL|UNITED STATES MARSHAL|SECRET SERVICE)\\b"), "FEDERAL_ENFORCEMENT", true),
            OwnerPattern(Regex("\\b(COAST GUARD|USCG)\\b"), "COAST_GUARD", true),
            OwnerPattern(Regex("\\b(STATE OF|CITY OF|COUNTY OF|COMMONWEALTH OF)\\b"), "GOV_OWNER", true),
            OwnerPattern(Regex("\\b(FEDERAL AVIATION ADMINISTRATION|FAA)\\b"), "FAA", true)
        )
    }

    private data class OwnerResult(val tag: String, val isGovernment: Boolean)

    private fun checkOwner(name: String): OwnerResult? {
        val normalized = name.replace(Regex("\\s+"), " ")
        for (pattern in ownerPatterns) {
            if (pattern.regex.containsMatchIn(normalized)) {
                return OwnerResult(pattern.tag, pattern.isGovernment)
            }
        }
        return null
    }

    // ---- Signal 3: Military Type Codes ----

    private data class TypeCodeResult(val tag: String, val score: Int)

    private val highConfidenceMilitaryTypeCodes: Set<String> by lazy {
        setOf(
            // Fighters
            "F16", "F15", "F18", "FA18", "F22", "F35",
            "F14", "F4", "F5",
            "EUFI", "RFAL", "GR4", "GRF4",
            "MIRA", "MIR2",
            "SU27", "SU30", "SU34", "SU35", "SU57", "SU25",
            "MIG29", "MIG31",
            "JF17", "J10", "J20", "FC31",   // FC31 = J-31 Gyrfalcon
            "WZ10",                           // Z-10 attack helicopter
            "KFIR",
            // Bombers
            "B1", "B1B", "B2", "B52", "B52H",
            "TU95", "TU160", "TU22", "TU16", // TU16 = H-6 (Chinese license)
            // Transport, tanker, and special mission aircraft with strong military signal
            "C17", "C17A", "C5", "C5M", "C2",
            "KC10", "KC46", "KC135",
            "A400", "A400M",
            "C160", "AN22",
            "IL76", "Y20",                    // Y-20 Chinese heavy transport
            // Helicopters (military)
            "H60", "UH60", "HH60", "MH60", "SH60",
            "AH64", "AH1", "AH1Z",
            "H47", "CH47", "CH53", "CH46",
            "V22", "MV22", "CV22",
            "NH90", "EH10", "LYNX",
            "MI8", "MI17", "MI24", "MI26", "MI28", "KA52",
            "S70",
            // Patrol / Recon / AWACS
            "E3", "E3A", "E3B", "E3CF",
            "E2", "E2C", "E2D",
            "E4B",
            "E6", "E6B",
            "E8", "E8C",
            "RC135",
            "P3", "P8", "P8A",
            "U2", "U2S",
            "SR71",
            // UAV (military)
            "RQ4", "MQ9", "MQ1", "RQ7", "RQ170",
            // Attack
            "AC130", "F117", "AV8B", "EA18",
            // Tankers / Special
            "A330MRTT", "MRTT"
        )
    }

    private val supportingMilitaryTypeCodes: Set<String> by lazy {
        setOf(
            // Dual-use, retired, or civilian-operated ex-military types.
            "C130", "C30J", "C295W",
            "AN12", "AN72", "AN124", "AN225",
            // Trainers
            "T6", "T38", "T45",
            "PC21", "PC7",
            "HAWK",
            // Misc
            "C12",
            "C295", "CN35"
        )
    }

    private fun checkTypeCode(typeCode: String): TypeCodeResult? {
        return when (typeCode) {
            in highConfidenceMilitaryTypeCodes -> TypeCodeResult(typeCode, score = 45)
            in supportingMilitaryTypeCodes -> TypeCodeResult(typeCode, score = 25)
            else -> null
        }
    }
}
