package com.friendorfoe.detection

import com.friendorfoe.domain.model.Drone
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap

/**
 * Accumulates standalone OpenDroneID messages and canonicalizes parsed states
 * by Basic ID. Atomic Message Packs always start from a clean state so two
 * virtual aircraft sharing one BLE transmitter cannot borrow each other's
 * location or operator fields.
 */
internal class BleRemoteIdPayloadProcessor {

    private val transportStates = ConcurrentHashMap<String, OpenDroneIdParser.DronePartialState>()
    private val canonicalStates = ConcurrentHashMap<String, OpenDroneIdParser.DronePartialState>()

    fun process(
        deviceAddress: String,
        payload: ByteArray,
        now: Instant,
        signalStrengthDbm: Int? = null,
        estimatedDistanceMeters: Double? = null
    ): Drone? {
        if (payload.isEmpty()) return null

        val messageType = (payload[0].toInt() and 0xF0) ushr 4
        val isMessagePack = messageType == OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK
        if (isMessagePack && !OpenDroneIdParser.isValidMessagePack(payload)) return null

        val parsedState = if (isMessagePack) {
            OpenDroneIdParser.DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
        } else {
            transportStates.getOrPut(deviceAddress) {
                OpenDroneIdParser.DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
            }
        }

        synchronized(parsedState) {
            parsedState.lastUpdated = now
            parsedState.signalStrengthDbm = signalStrengthDbm
            parsedState.estimatedDistanceMeters = estimatedDistanceMeters
            OpenDroneIdParser.parseMessage(payload, parsedState)

            val serial = parsedState.droneId ?: return null
            val canonicalState = canonicalStates.getOrPut(serial) {
                OpenDroneIdParser.DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
            }

            synchronized(canonicalState) {
                canonicalState.copyParsedFieldsFrom(parsedState)
                return canonicalState.toDroneOrNull(idPrefix = "rid_")
            }
        }
    }

    fun clear() {
        transportStates.clear()
        canonicalStates.clear()
    }

    private fun OpenDroneIdParser.DronePartialState.copyParsedFieldsFrom(
        source: OpenDroneIdParser.DronePartialState
    ) {
        lastUpdated = source.lastUpdated
        droneId = source.droneId
        uaType = source.uaType
        latitude = source.latitude
        longitude = source.longitude
        altitudeMeters = source.altitudeMeters
        heading = source.heading
        speedMps = source.speedMps
        operatorLatitude = source.operatorLatitude
        operatorLongitude = source.operatorLongitude
        operatorId = source.operatorId
        signalStrengthDbm = source.signalStrengthDbm
        rttDistanceMeters = source.rttDistanceMeters
        estimatedDistanceMeters = source.estimatedDistanceMeters
        selfIdText = source.selfIdText
        selfIdDescriptionType = source.selfIdDescriptionType
        verticalSpeedMps = source.verticalSpeedMps
        geodeticAltitudeMeters = source.geodeticAltitudeMeters
        heightAglMeters = source.heightAglMeters
        horizontalAccuracyCode = source.horizontalAccuracyCode
        verticalAccuracyCode = source.verticalAccuracyCode
        locationTimestamp = source.locationTimestamp
        idType = source.idType
        areaCount = source.areaCount
        areaRadius = source.areaRadius
        areaCeiling = source.areaCeiling
        areaFloor = source.areaFloor
        classificationTypeCode = source.classificationTypeCode
    }
}
