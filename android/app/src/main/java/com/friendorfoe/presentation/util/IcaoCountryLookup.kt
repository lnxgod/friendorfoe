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
        Triple(0x880000L, 0x887FFFL, "India"),
        // Middle East
        Triple(0x4B8000L, 0x4BFFFFL, "Turkey"),
        Triple(0x710000L, 0x717FFFL, "Saudi Arabia"),
        Triple(0x896000L, 0x896FFFL, "United Arab Emirates"),
        Triple(0x06A000L, 0x06A3FFL, "Qatar"),
        Triple(0x894000L, 0x894FFFL, "Bahrain"),
        Triple(0x70C000L, 0x70C3FFL, "Oman"),
        // South & Southeast Asia
        Triple(0x760000L, 0x767FFFL, "Pakistan"),
        Triple(0x8A0000L, 0x8A7FFFL, "Indonesia"),
        Triple(0x880000L, 0x88FFFFL, "Thailand"),
        Triple(0x750000L, 0x757FFFL, "Malaysia"),
        Triple(0x768000L, 0x76FFFFL, "Singapore"),
        Triple(0x758000L, 0x75FFFFL, "Philippines"),
        Triple(0x888000L, 0x88FFFFL, "Vietnam"),
        Triple(0x702000L, 0x702FFFL, "Bangladesh"),
        Triple(0x899000L, 0x8993FFL, "Taiwan"),
        // Oceania
        Triple(0xC80000L, 0xC87FFFL, "New Zealand"),
        // Latin America
        Triple(0xE00000L, 0xE3FFFFL, "Argentina"),
        Triple(0xE84000L, 0xE84FFFL, "Chile"),
        Triple(0x0AC000L, 0x0ACFFFL, "Colombia"),
        Triple(0xE8C000L, 0xE8CFFFL, "Peru"),
        Triple(0x0D8000L, 0x0DFFFFL, "Venezuela"),
        // Africa
        Triple(0x008000L, 0x00FFFFL, "South Africa"),
        Triple(0x010000L, 0x017FFFL, "Egypt"),
        Triple(0x020000L, 0x027FFFL, "Morocco"),
        Triple(0x064000L, 0x064FFFL, "Nigeria"),
        Triple(0x04C000L, 0x04CFFFL, "Kenya"),
        Triple(0x0A0000L, 0x0A7FFFL, "Algeria"),
        // Eastern Europe
        Triple(0x488000L, 0x48FFFFL, "Poland"),
        Triple(0x498000L, 0x49FFFFL, "Czech Republic"),
        Triple(0x4A0000L, 0x4A7FFFL, "Romania"),
        Triple(0x470000L, 0x477FFFL, "Hungary"),
        Triple(0x508000L, 0x50FFFFL, "Ukraine"),
        Triple(0x501C00L, 0x501FFFL, "Croatia"),
        Triple(0x450000L, 0x457FFFL, "Bulgaria"),
        // Nordics
        Triple(0x4A8000L, 0x4AFFFFL, "Sweden"),
        Triple(0x478000L, 0x47FFFFL, "Norway"),
        Triple(0x458000L, 0x45FFFFL, "Denmark"),
        Triple(0x460000L, 0x467FFFL, "Finland"),
        Triple(0x4CC000L, 0x4CCFFFL, "Iceland"),
        // Western Europe (additional)
        Triple(0x4CA000L, 0x4CAFFFL, "Ireland"),
        Triple(0x490000L, 0x497FFFL, "Portugal"),
        Triple(0x468000L, 0x46FFFFL, "Greece"),
        Triple(0x4B0000L, 0x4B7FFFL, "Switzerland")
    )

    fun countryFromIcaoHex(icaoHex: String): String? {
        val value = icaoHex.trim().lowercase().toLongOrNull(16) ?: return null
        return RANGES.firstOrNull { (low, high, _) -> value in low..high }?.third
    }
}
