package com.friendorfoe.presentation.util

/**
 * Derives registration country from ICAO 24-bit hex address ranges.
 *
 * Ported from backend enrichment service (app/services/enrichment.py).
 */
object IcaoCountryLookup {
    private val RANGES = listOf(
        Triple(0xA00000L, 0xAFFFFFL, "United States"),
        Triple(0xC00000L, 0xC3FFFFL, "Canada"),
        Triple(0x400000L, 0x43FFFFL, "United Kingdom"),
        Triple(0x3C0000L, 0x3FFFFFL, "Germany"),
        Triple(0x380000L, 0x3BFFFFL, "France"),
        Triple(0x300000L, 0x33FFFFL, "Italy"),
        Triple(0x340000L, 0x37FFFFL, "Spain"),
        Triple(0x840000L, 0x87FFFFL, "China"),
        Triple(0x780000L, 0x7BFFFFL, "Japan"),
        Triple(0x7C0000L, 0x7FFFFFL, "Australia"),
        Triple(0x500000L, 0x5003FFL, "Israel"),
        Triple(0x710000L, 0x717FFFL, "South Korea"),
        Triple(0x480000L, 0x4BFFFFL, "Netherlands"),
        Triple(0x440000L, 0x447FFFL, "Austria"),
        Triple(0x4C0000L, 0x4FFFFFL, "Belgium"),
        Triple(0x0C0000L, 0x0FFFFFL, "Mexico"),
        Triple(0xE00000L, 0xE3FFFFL, "Brazil"),
        Triple(0x600000L, 0x6003FFL, "Russia"),
        Triple(0x880000L, 0x887FFFL, "India")
    )

    fun countryFromIcaoHex(icaoHex: String): String? {
        val value = icaoHex.trim().lowercase().toLongOrNull(16) ?: return null
        return RANGES.firstOrNull { (low, high, _) -> value in low..high }?.third
    }
}
