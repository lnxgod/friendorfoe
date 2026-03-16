package com.friendorfoe.detection

/**
 * Converts WiFi center frequency (MHz) to human-readable band + channel labels.
 */
object WifiChannelUtil {

    /**
     * Convert WiFi center frequency to a readable band + channel string.
     *
     * Examples:
     *   2412 → "2.4 GHz Ch 1"
     *   2437 → "2.4 GHz Ch 6"
     *   5180 → "5 GHz Ch 36"
     *   5745 → "5 GHz Ch 149"
     *   5955 → "6 GHz Ch 1"
     */
    fun frequencyToChannelLabel(freqMhz: Int): String {
        return when {
            // 2.4 GHz band (2412-2484)
            freqMhz in 2412..2472 -> {
                val ch = (freqMhz - 2412) / 5 + 1
                "2.4 GHz Ch $ch"
            }
            freqMhz == 2484 -> "2.4 GHz Ch 14"

            // 5 GHz band (5170-5885)
            freqMhz in 5170..5885 -> {
                val ch = (freqMhz - 5000) / 5
                "5 GHz Ch $ch"
            }

            // 6 GHz band (5955-7115)
            freqMhz in 5955..7115 -> {
                val ch = (freqMhz - 5950) / 5
                "6 GHz Ch $ch"
            }

            else -> "$freqMhz MHz"
        }
    }
}
