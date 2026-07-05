package com.friendorfoe.detection

import kotlin.math.pow

object RssiDistanceEstimator {
    private const val FSPL_2_4_GHZ_AT_1M_DB = 40.05
    private const val WIFI_DRONE_DEFAULT_TX_POWER_DBM = 13.2
    private const val WIFI_DRONE_PATH_LOSS = 2.4
    private const val BLE_REMOTE_ID_DEFAULT_TX_POWER_DBM = 1.8
    private const val BLE_REMOTE_ID_PATH_LOSS = 2.3

    fun estimateWifiDrone(rssi: Int, txPowerDbm: Int? = null): Double =
        estimate(
            rssi = rssi,
            referenceRssiAtOneMeter = referenceRssiAtOneMeter(
                txPowerDbm?.toDouble() ?: WIFI_DRONE_DEFAULT_TX_POWER_DBM
            ),
            pathLossExponent = WIFI_DRONE_PATH_LOSS,
            minMeters = 1.0,
            maxMeters = 10000.0
        )

    fun estimateBleRemoteId(rssi: Int, txPowerDbm: Int? = null): Double =
        estimate(
            rssi = rssi,
            referenceRssiAtOneMeter = referenceRssiAtOneMeter(
                txPowerDbm?.toDouble() ?: BLE_REMOTE_ID_DEFAULT_TX_POWER_DBM
            ),
            pathLossExponent = BLE_REMOTE_ID_PATH_LOSS,
            minMeters = 0.5,
            maxMeters = 5000.0
        )

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

    private fun referenceRssiAtOneMeter(txPowerDbm: Double): Double =
        txPowerDbm - FSPL_2_4_GHZ_AT_1M_DB
}
