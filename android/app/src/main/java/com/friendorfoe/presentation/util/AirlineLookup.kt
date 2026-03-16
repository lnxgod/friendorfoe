package com.friendorfoe.presentation.util

/**
 * Resolves airline names from ICAO 3-letter callsign prefixes.
 *
 * Ported from backend enrichment service (app/services/enrichment.py).
 */
object AirlineLookup {
    data class AirlineInfo(val name: String, val iataCode: String)

    private val PREFIXES = mapOf(
        "AAL" to AirlineInfo("American Airlines", "AA"),
        "UAL" to AirlineInfo("United Airlines", "UA"),
        "DAL" to AirlineInfo("Delta Air Lines", "DL"),
        "SWA" to AirlineInfo("Southwest Airlines", "WN"),
        "JBU" to AirlineInfo("JetBlue Airways", "B6"),
        "SKW" to AirlineInfo("SkyWest Airlines", "OO"),
        "ASA" to AirlineInfo("Alaska Airlines", "AS"),
        "NKS" to AirlineInfo("Spirit Airlines", "NK"),
        "FFT" to AirlineInfo("Frontier Airlines", "F9"),
        "RPA" to AirlineInfo("Republic Airways", "YX"),
        "ENY" to AirlineInfo("Envoy Air", "MQ"),
        "BAW" to AirlineInfo("British Airways", "BA"),
        "DLH" to AirlineInfo("Lufthansa", "LH"),
        "AFR" to AirlineInfo("Air France", "AF"),
        "KLM" to AirlineInfo("KLM Royal Dutch", "KL"),
        "EZY" to AirlineInfo("easyJet", "U2"),
        "RYR" to AirlineInfo("Ryanair", "FR"),
        "QFA" to AirlineInfo("Qantas", "QF"),
        "ACA" to AirlineInfo("Air Canada", "AC"),
        "ANZ" to AirlineInfo("Air New Zealand", "NZ"),
        "SIA" to AirlineInfo("Singapore Airlines", "SQ"),
        "CPA" to AirlineInfo("Cathay Pacific", "CX"),
        "ANA" to AirlineInfo("All Nippon Airways", "NH"),
        "JAL" to AirlineInfo("Japan Airlines", "JL"),
        "UAE" to AirlineInfo("Emirates", "EK"),
        "ETH" to AirlineInfo("Ethiopian Airlines", "ET"),
        "THY" to AirlineInfo("Turkish Airlines", "TK"),
        "CSN" to AirlineInfo("China Southern", "CZ"),
        "CCA" to AirlineInfo("Air China", "CA"),
        "CES" to AirlineInfo("China Eastern", "MU")
    )

    fun matchByCallsign(callsign: String): AirlineInfo? {
        if (callsign.length < 4) return null
        return PREFIXES[callsign.take(3).uppercase()]
    }
}
