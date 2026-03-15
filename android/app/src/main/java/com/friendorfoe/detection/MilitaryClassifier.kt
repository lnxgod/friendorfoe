package com.friendorfoe.detection

import com.friendorfoe.domain.model.ObjectCategory

/**
 * Multi-signal scoring classifier for military and government aircraft.
 *
 * Uses three signal types from ADS-B data:
 * 1. ICAO hex address ranges (known military allocations)
 * 2. Callsign patterns (military/government call prefixes)
 * 3. ICAO type designator codes (military aircraft types)
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
        registration: String?
    ): ClassificationResult {
        val signals = mutableListOf<String>()
        var score = 0
        var isGovernment = false

        // Signal 1: ICAO hex range
        if (icaoHex != null) {
            val hexResult = checkIcaoHex(icaoHex.lowercase())
            if (hexResult != null) {
                score += 40
                signals.add("ICAO:${hexResult.tag}")
                if (hexResult.isGovernment) isGovernment = true
            }
        }

        // Signal 2: Callsign pattern
        if (callsign != null) {
            val csResult = checkCallsign(callsign.uppercase().trim())
            if (csResult != null) {
                score += 35
                signals.add("CALLSIGN:${csResult.tag}")
                if (csResult.isGovernment) isGovernment = true
            }
        }

        // Signal 3: Type code
        if (typeCode != null) {
            val typeResult = checkTypeCode(typeCode.uppercase().trim())
            if (typeResult != null) {
                score += 25
                signals.add("TYPE:${typeResult}")
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
            // USA — Government/FAA
            HexRangeEntry(0xA00000, 0xA00FFF, "US_GOV_FAA", isGovernment = true),
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
        val isGovernment: Boolean = false
    )

    private val callsignPatterns: List<CallsignPattern> by lazy {
        listOf(
            // US Military
            CallsignPattern(Regex("^RCH\\d+"), "REACH_AMC"),
            CallsignPattern(Regex("^EVAC\\d*"), "EVAC"),
            CallsignPattern(Regex("^DUKE\\d+"), "DUKE"),
            CallsignPattern(Regex("^NAVY\\d*"), "NAVY"),
            CallsignPattern(Regex("^ARMY\\d*"), "ARMY"),
            CallsignPattern(Regex("^TOPCAT\\d*"), "TOPCAT"),
            CallsignPattern(Regex("^TEAL\\d+"), "TEAL"),
            CallsignPattern(Regex("^ORDER\\d+"), "ORDER"),
            CallsignPattern(Regex("^MOOSE\\d+"), "MOOSE"),
            CallsignPattern(Regex("^PACK\\d+"), "PACK"),
            CallsignPattern(Regex("^RAGE\\d+"), "RAGE"),
            CallsignPattern(Regex("^HAWK\\d+"), "HAWK"),
            CallsignPattern(Regex("^VIPER\\d+"), "VIPER"),
            CallsignPattern(Regex("^COBRA\\d+"), "COBRA"),
            CallsignPattern(Regex("^SKULL\\d+"), "SKULL"),
            CallsignPattern(Regex("^KNIFE\\d+"), "KNIFE"),
            CallsignPattern(Regex("^TABOO\\d+"), "TABOO"),
            CallsignPattern(Regex("^SNTRY\\d*"), "SENTRY"),
            CallsignPattern(Regex("^GUCCI\\d+"), "GUCCI"),
            CallsignPattern(Regex("^ROCKY\\d+"), "ROCKY"),
            CallsignPattern(Regex("^STONE\\d+"), "STONE"),
            // Generic US military prefixes
            CallsignPattern(Regex("^AF[0-9]"), "USAF"),
            CallsignPattern(Regex("^MC[0-9]"), "USMC"),
            // International military
            CallsignPattern(Regex("^BAF\\d+"), "BELGIAN_AF"),
            CallsignPattern(Regex("^GAF\\d+"), "GERMAN_AF"),
            CallsignPattern(Regex("^FAF\\d+"), "FRENCH_AF"),
            CallsignPattern(Regex("^RAF\\d+"), "ROYAL_AF"),
            CallsignPattern(Regex("^IAF\\d+"), "ISRAELI_AF"),
            CallsignPattern(Regex("^IAM\\d+"), "ITALIAN_AF"),
            CallsignPattern(Regex("^SUI\\d+"), "SWISS_AF"),
            CallsignPattern(Regex("^NOR\\d+"), "NORWEGIAN_AF"),
            CallsignPattern(Regex("^DAF\\d+"), "DANISH_AF"),
            CallsignPattern(Regex("^PLF\\d+"), "POLISH_AF"),
            CallsignPattern(Regex("^HAF\\d+"), "GREEK_AF"),
            CallsignPattern(Regex("^TKF\\d+"), "TURKISH_AF"),
            CallsignPattern(Regex("^SWF\\d+"), "SWEDISH_AF"),
            CallsignPattern(Regex("^RNF\\d+"), "DUTCH_AF"),
            CallsignPattern(Regex("^CNF\\d+"), "CANADIAN_AF"),
            CallsignPattern(Regex("^ASF\\d+"), "AUSTRALIAN_AF"),
            // Government / law enforcement
            CallsignPattern(Regex("^EXEC\\d*"), "EXECUTIVE", isGovernment = true),
            CallsignPattern(Regex("^SAMP\\d+"), "SAM_PRIORITY", isGovernment = true),
            CallsignPattern(Regex("^COAST\\d+"), "COAST_GUARD", isGovernment = true),
            CallsignPattern(Regex("^CBP\\d+"), "CBP", isGovernment = true),
            CallsignPattern(Regex("^PAT\\d+"), "BORDER_PATROL", isGovernment = true),
            CallsignPattern(Regex("^COPTER\\d+"), "LAW_ENFORCEMENT", isGovernment = true),
            // Emergency / medevac
            CallsignPattern(Regex("^LIFEGUARD"), "LIFEGUARD"),
            CallsignPattern(Regex("^MEDEVAC"), "MEDEVAC"),
        )
    }

    private data class CallsignResult(val tag: String, val isGovernment: Boolean)

    private fun checkCallsign(callsign: String): CallsignResult? {
        for (pattern in callsignPatterns) {
            if (pattern.regex.containsMatchIn(callsign)) {
                return CallsignResult(tag = pattern.tag, isGovernment = pattern.isGovernment)
            }
        }
        return null
    }

    // ---- Signal 3: Military Type Codes ----

    private val militaryTypeCodes: Set<String> by lazy {
        setOf(
            // Fighters
            "F16", "F15", "F18", "FA18", "F22", "F35",
            "F14", "F4", "F5",
            "EUFI", "RFAL", "GR4", "GRF4",
            "MIRA", "MIR2",
            "SU27", "SU30", "SU34", "SU35", "MIG29", "MIG31",
            "JF17", "J10", "J20",
            "KFIR",
            // Bombers
            "B1", "B1B", "B2", "B52", "B52H",
            "TU95", "TU160", "TU22",
            // Transport
            "C130", "C17", "C5", "C5M", "C2",
            "KC10", "KC46", "KC135",
            "A400", "A400M",
            "C160", "AN12", "AN22", "AN124", "AN225",
            "IL76",
            // Helicopters (military)
            "H60", "UH60", "HH60", "MH60", "SH60",
            "AH64", "AH1", "AH1Z",
            "CH47", "CH53", "CH46",
            "V22", "MV22", "CV22",
            "NH90", "EH10", "LYNX",
            "MI8", "MI17", "MI24", "MI26", "MI28", "KA52",
            "S70",
            // Patrol / Recon / AWACS
            "E3", "E3A", "E3B",
            "E2", "E2C", "E2D",
            "E6", "E6B",
            "E8", "E8C",
            "RC135",
            "P3", "P8", "P8A",
            "U2", "U2S",
            "SR71",
            // UAV (military)
            "RQ4", "MQ9", "MQ1", "RQ7", "RQ170",
            // Trainers
            "T6", "T38", "T45",
            "PC21", "PC7",
            "HAWK",
            // Tankers / Special
            "A330MRTT", "MRTT",
            "C295", "CN35"
        )
    }

    private fun checkTypeCode(typeCode: String): String? {
        return if (typeCode in militaryTypeCodes) typeCode else null
    }
}
