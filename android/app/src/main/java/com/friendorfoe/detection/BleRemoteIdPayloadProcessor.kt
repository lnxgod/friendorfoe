package com.friendorfoe.detection

import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.isFormationDroneId
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap

/**
 * Accumulates standalone OpenDroneID messages and canonicalizes parsed states
 * by Basic ID. Atomic Message Packs always start from a clean state so two
 * virtual aircraft sharing one BLE transmitter cannot borrow each other's
 * location or operator fields.
 */
internal class BleRemoteIdPayloadProcessor {

    private data class FormationTransaction(
        val serial: String,
        val transportCounter: Int,
        val startedObservationNanos: Long,
        var lastMessageType: Int = OpenDroneIdParser.MSG_TYPE_BASIC_ID,
        var lastObservationNanos: Long = startedObservationNanos
    ) {
        fun record(messageType: Int, observationNanos: Long) {
            lastMessageType = messageType
            lastObservationNanos = observationNanos
        }

        fun isRecentDuplicate(messageType: Int, observationNanos: Long): Boolean {
            val elapsed = observationNanos - lastObservationNanos
            return lastMessageType == messageType && elapsed >= 0L &&
                elapsed <= FORMATION_DUPLICATE_WINDOW_NANOS
        }
    }

    private val transportStates = ConcurrentHashMap<String, OpenDroneIdParser.DronePartialState>()
    private val canonicalStates = ConcurrentHashMap<String, OpenDroneIdParser.DronePartialState>()
    private val formationTransactions = ConcurrentHashMap<String, FormationTransaction>()

    fun process(
        deviceAddress: String,
        payload: ByteArray,
        now: Instant,
        signalStrengthDbm: Int? = null,
        estimatedDistanceMeters: Double? = null,
        transportCounter: Int? = null,
        observationTimestampNanos: Long = now.toEpochMilli() * NANOS_PER_MILLISECOND
    ): Drone? {
        if (payload.isEmpty()) return null

        val messageType = (payload[0].toInt() and 0xF0) ushr 4
        val isMessagePack = messageType == OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK
        if (isMessagePack && !OpenDroneIdParser.isValidMessagePack(payload)) return null
        if (isMessagePack) formationTransactions.remove(deviceAddress)
        if (!isMessagePack && messageType != OpenDroneIdParser.MSG_TYPE_BASIC_ID) {
            val transaction = formationTransactions[deviceAddress]
            if (transaction != null && transaction.transportCounter != transportCounter) {
                return null
            }
        }

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
            if (!isMessagePack && messageType == OpenDroneIdParser.MSG_TYPE_BASIC_ID) {
                // Basic parsing leaves the accumulator unchanged for a blank
                // or malformed ID. Clear it first so the current frame cannot
                // accidentally reopen the preceding formation identity.
                parsedState.droneId = null
            }
            OpenDroneIdParser.parseMessage(payload, parsedState)

            val parsedSerial = parsedState.droneId
            if (!isMessagePack && messageType == OpenDroneIdParser.MSG_TYPE_BASIC_ID) {
                if (parsedSerial != null && isFormationDroneId(parsedSerial)) {
                    if (transportCounter == null) {
                        parsedState.clearAircraftFieldsForFormationTransaction()
                        formationTransactions.remove(deviceAddress)
                        return null
                    }
                    val existing = formationTransactions[deviceAddress]
                    if (existing != null && existing.serial == parsedSerial &&
                        existing.transportCounter == transportCounter &&
                        !existing.isExpiredAt(observationTimestampNanos) &&
                        existing.isRecentDuplicate(messageType, observationTimestampNanos)) {
                        // The C5 holds each payload for more than one radio
                        // interval. Preserve progress across a duplicate Basic.
                        return null
                    }
                    // Every formation Basic ID begins a new point transaction,
                    // clearing shared-MAC fields. System and Operator ID are
                    // optional; a matching Location commits the transaction.
                    parsedState.clearAircraftFieldsForFormationTransaction()
                    formationTransactions[deviceAddress] = FormationTransaction(
                        serial = parsedSerial,
                        transportCounter = transportCounter,
                        startedObservationNanos = observationTimestampNanos
                    )
                    return null
                }
                formationTransactions.remove(deviceAddress)
            }

            val serial = parsedState.droneId ?: return null
            if (!isMessagePack && isFormationDroneId(serial)) {
                val transaction = formationTransactions[deviceAddress] ?: return null
                if (transaction.serial != serial ||
                    transaction.transportCounter != transportCounter ||
                    transaction.isExpiredAt(observationTimestampNanos)) {
                    formationTransactions.remove(deviceAddress)
                    return null
                }
                when (messageType) {
                    OpenDroneIdParser.MSG_TYPE_SYSTEM,
                    OpenDroneIdParser.MSG_TYPE_OPERATOR_ID -> {
                        transaction.record(messageType, observationTimestampNanos)
                        return null
                    }
                    OpenDroneIdParser.MSG_TYPE_LOCATION -> {
                        if (parsedState.latitude == null || parsedState.longitude == null) {
                            formationTransactions.remove(deviceAddress)
                            return null
                        }
                        formationTransactions.remove(deviceAddress)
                    }
                    else -> {
                        formationTransactions.remove(deviceAddress)
                        return null
                    }
                }
            }
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
        formationTransactions.clear()
    }

    private fun FormationTransaction.isExpiredAt(observationNanos: Long): Boolean {
        val elapsed = observationNanos - startedObservationNanos
        return elapsed < 0L || elapsed > FORMATION_TRANSACTION_TIMEOUT_NANOS
    }

    /**
     * One BLE advertiser may carousel several simulated Basic IDs. Once the
     * ID changes, location and identity-specific fields from the previous
     * aircraft must not follow the new ID while its next messages arrive.
     */
    private fun OpenDroneIdParser.DronePartialState.clearAircraftFieldsForFormationTransaction() {
        latitude = null
        longitude = null
        altitudeMeters = null
        heading = null
        speedMps = null
        operatorLatitude = null
        operatorLongitude = null
        operatorId = null
        rttDistanceMeters = null
        selfIdText = null
        selfIdDescriptionType = null
        verticalSpeedMps = null
        geodeticAltitudeMeters = null
        heightAglMeters = null
        horizontalAccuracyCode = null
        verticalAccuracyCode = null
        locationTimestamp = null
        areaCount = null
        areaRadius = null
        areaCeiling = null
        areaFloor = null
        classificationTypeCode = null
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

    private companion object {
        const val NANOS_PER_MILLISECOND = 1_000_000L
        const val FORMATION_DUPLICATE_WINDOW_NANOS = 250L * NANOS_PER_MILLISECOND
        const val FORMATION_TRANSACTION_TIMEOUT_NANOS = 2_000L * NANOS_PER_MILLISECOND
    }
}
