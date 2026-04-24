package com.friendorfoe.calibration

interface CalibrationAdvertiser {
    suspend fun start(serviceUuid: String): Result<Unit>
    fun stop()
}
