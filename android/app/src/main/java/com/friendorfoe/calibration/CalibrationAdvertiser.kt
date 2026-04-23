package com.friendorfoe.calibration

interface CalibrationAdvertiser {
    fun start(serviceUuid: String, onError: (String) -> Unit = {}): Boolean
    fun stop()
}
