package com.friendorfoe.data.remote

class FakeSensorMapApiService : SensorMapApiService {
    var droneMap = DroneMapDto()
    var sensors = SensorsDto()
    var nodesStatus = NodeStatusDto()
    var droneAlerts = DroneAlertsDto()
    var events = EventsDto()
    var eventStats = EventStatsDto()
    var probeDevices = ProbeDevicesDto()
    var wifiApInventory = WifiApInventoryDto()
    var calibrationModel = CalibrationModelDto()
    var health = HealthDto(status = "ok", version = "test", redis = "ok", database = "ok")
    var ackedEventIds = mutableListOf<Int>()

    override suspend fun getDroneMap(): DroneMapDto = droneMap
    override suspend fun getSensors(): SensorsDto = sensors
    override suspend fun getNodesStatus(): NodeStatusDto = nodesStatus
    override suspend fun getDroneAlerts(): DroneAlertsDto = droneAlerts
    override suspend fun getEvents(
        types: List<String>?,
        acknowledged: Boolean?,
        sinceHours: Float,
        limit: Int,
    ): EventsDto = events

    override suspend fun getEventStats(): EventStatsDto = eventStats

    override suspend fun ackEvent(eventId: Int): AckResponseDto {
        ackedEventIds.add(eventId)
        return AckResponseDto(ok = true, eventId = eventId)
    }

    override suspend fun getProbeDevices(maxAgeS: Int, droneOnly: Boolean): ProbeDevicesDto = probeDevices
    override suspend fun getWifiApInventory(maxAgeS: Int): WifiApInventoryDto = wifiApInventory
    override suspend fun getCalibrationModel(): CalibrationModelDto = calibrationModel
    override suspend fun getHealth(): HealthDto = health
}
