package com.friendorfoe.detection

import kotlin.math.pow

object RssiDistanceEstimator {
    // These are calibrated 1m references. Advertised TX power is not a
    // replacement for that calibration; it only adjusts observed path loss.
    private const val WIFI_DRONE_REFERENCE_RSSI_AT_ONE_METER_DBM = -55.0
    private const val WIFI_DRONE_REFERENCE_TX_POWER_DBM = 15.0
    private const val WIFI_DRONE_PATH_LOSS = 2.3
    private const val BLE_REMOTE_ID_REFERENCE_RSSI_AT_ONE_METER_DBM = -59.0
    private const val BLE_REMOTE_ID_REFERENCE_TX_POWER_DBM = 0.0
    private const val BLE_REMOTE_ID_PATH_LOSS = 2.0

    private val WIFI_TX_POWER_PLAUSIBLE_RANGE = -10..30
    private val BLE_TX_POWER_PLAUSIBLE_RANGE = -40..20

    fun estimateWifiDrone(rssi: Int, txPowerDbm: Int? = null): Double =
        estimateWithProfile(
            rssi = rssi,
            advertisedTxPowerDbm = txPowerDbm,
            referenceRssiAtOneMeter = WIFI_DRONE_REFERENCE_RSSI_AT_ONE_METER_DBM,
            referenceTxPowerDbm = WIFI_DRONE_REFERENCE_TX_POWER_DBM,
            pathLossExponent = WIFI_DRONE_PATH_LOSS,
            minMeters = 0.5,
            maxMeters = 5000.0,
            plausibleTxPowerRange = WIFI_TX_POWER_PLAUSIBLE_RANGE
        )

    fun estimateBleRemoteId(rssi: Int, txPowerDbm: Int? = null): Double =
        estimateWithProfile(
            rssi = rssi,
            advertisedTxPowerDbm = txPowerDbm,
            referenceRssiAtOneMeter = BLE_REMOTE_ID_REFERENCE_RSSI_AT_ONE_METER_DBM,
            referenceTxPowerDbm = BLE_REMOTE_ID_REFERENCE_TX_POWER_DBM,
            pathLossExponent = BLE_REMOTE_ID_PATH_LOSS,
            minMeters = 0.1,
            maxMeters = 1000.0,
            plausibleTxPowerRange = BLE_TX_POWER_PLAUSIBLE_RANGE
        )

    private fun estimateWithProfile(
        rssi: Int,
        advertisedTxPowerDbm: Int?,
        referenceRssiAtOneMeter: Double,
        referenceTxPowerDbm: Double,
        pathLossExponent: Double,
        minMeters: Double,
        maxMeters: Double,
        plausibleTxPowerRange: IntRange
    ): Double {
        val txPowerDbm = advertisedTxPowerDbm
            ?.takeIf { it in plausibleTxPowerRange }
            ?.toDouble()
            ?: referenceTxPowerDbm
        val referencePathLoss = referenceTxPowerDbm - referenceRssiAtOneMeter
        val observedPathLoss = txPowerDbm - rssi
        val exponent = (observedPathLoss - referencePathLoss) / (10.0 * pathLossExponent)
        return 10.0.pow(exponent).coerceIn(minMeters, maxMeters)
    }

    fun estimate(
        rssi: Int,
        referenceRssiAtOneMeter: Double,
        pathLossExponent: Double,
        minMeters: Double,
        maxMeters: Double
    ): Double {
        val exponent = (referenceRssiAtOneMeter - rssi) / (10.0 * pathLossExponent)
        return 10.0.pow(exponent).coerceIn(minMeters, maxMeters)
    }
}
