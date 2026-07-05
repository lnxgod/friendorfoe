package com.friendorfoe.detection

import kotlin.math.pow

object RssiDistanceEstimator {
    private const val WIFI_DRONE_RSSI_REF_DBM = -55.0
    private const val WIFI_DRONE_PATH_LOSS = 2.3
    private const val BLE_REMOTE_ID_RSSI_REF_DBM = -59.0
    private const val BLE_REMOTE_ID_PATH_LOSS = 2.0

    fun estimateWifiDrone(rssi: Int): Double =
        estimate(
            rssi = rssi,
            referenceRssiAtOneMeter = WIFI_DRONE_RSSI_REF_DBM,
            pathLossExponent = WIFI_DRONE_PATH_LOSS,
            minMeters = 0.5,
            maxMeters = 5000.0
        )

    fun estimateBleRemoteId(rssi: Int): Double =
        estimate(
            rssi = rssi,
            referenceRssiAtOneMeter = BLE_REMOTE_ID_RSSI_REF_DBM,
            pathLossExponent = BLE_REMOTE_ID_PATH_LOSS,
            minMeters = 0.1,
            maxMeters = 1000.0
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
}
