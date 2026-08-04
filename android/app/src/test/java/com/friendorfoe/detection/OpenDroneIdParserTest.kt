package com.friendorfoe.detection

import com.friendorfoe.domain.model.DetectionSource
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant

class OpenDroneIdParserTest {

    @Test
    fun remoteIdStateEmitsDroneWithPositionAndRssiRange() {
        val state = OpenDroneIdParser.DronePartialState(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            firstSeen = Instant.parse("2026-07-04T12:00:00Z")
        )

        OpenDroneIdParser.parseMessage(basicIdMessage("SERIAL123"), state)
        OpenDroneIdParser.parseMessage(locationMessage(37.7749, -122.4194), state)
        state.signalStrengthDbm = -79
        state.estimatedDistanceMeters = RssiDistanceEstimator.estimateBleRemoteId(-79)

        val drone = state.toDroneOrNull()

        assertNotNull(drone)
        requireNotNull(drone)
        assertEquals(DetectionSource.REMOTE_ID, drone.source)
        assertEquals("SERIAL123", drone.droneId)
        assertEquals(37.7749, drone.position.latitude, 0.0001)
        assertEquals(-122.4194, drone.position.longitude, 0.0001)
        assertEquals(-79, drone.signalStrengthDbm)
        assertEquals(10.0, drone.estimatedDistanceMeters ?: -1.0, 0.25)
    }

    @Test
    fun parsesStandardFourMessagePack() {
        val state = newState()
        val pack = messagePack(
            basicIdMessage("FOF-C5-A1B2C3-001"),
            locationMessage(36.13094, -115.15064),
            systemMessage(36.13100, -115.15050),
            operatorIdMessage("FOF-SIMULATOR")
        )

        assertEquals(103, pack.size)
        OpenDroneIdParser.parseMessage(pack, state)

        assertEquals("FOF-C5-A1B2C3-001", state.droneId)
        assertEquals(36.13094, state.latitude ?: 0.0, 0.000001)
        assertEquals(-115.15064, state.longitude ?: 0.0, 0.000001)
        assertEquals(10, state.horizontalAccuracyCode)
        assertEquals(4, state.verticalAccuracyCode)
        assertEquals(36.13100, state.operatorLatitude ?: 0.0, 0.000001)
        assertEquals(-115.15050, state.operatorLongitude ?: 0.0, 0.000001)
        assertEquals("FOF-SIMULATOR", state.operatorId)
        assertEquals(70, state.areaRadius)
        assertEquals(120.0, state.areaCeiling ?: 0.0, 0.001)
        assertEquals(80.0, state.areaFloor ?: 0.0, 0.001)
        assertEquals(3, state.classificationTypeCode)
    }

    @Test
    fun rejectsPackWithWrongSingleMessageSizeWithoutPartialMutation() {
        val state = newState()
        val pack = messagePack(
            basicIdMessage("FOF-C5-A1B2C3-001"),
            locationMessage(36.13094, -115.15064)
        )
        pack[1] = 24

        OpenDroneIdParser.parseMessage(pack, state)

        assertNull(state.droneId)
        assertNull(state.latitude)
        assertNull(state.longitude)
    }

    @Test
    fun rejectsTruncatedPackWithoutParsingCompletePrefixMessages() {
        val state = newState()
        val complete = messagePack(
            basicIdMessage("FOF-C5-A1B2C3-001"),
            locationMessage(36.13094, -115.15064)
        )
        val truncated = complete.copyOf(complete.size - 1)

        OpenDroneIdParser.parseMessage(truncated, state)

        assertNull(state.droneId)
        assertNull(state.latitude)
        assertNull(state.longitude)
    }

    private fun newState() = OpenDroneIdParser.DronePartialState(
        deviceAddress = "AA:BB:CC:DD:EE:FF",
        firstSeen = Instant.parse("2026-07-04T12:00:00Z")
    )

    private fun basicIdMessage(serial: String): ByteArray {
        val data = ByteArray(25)
        data[0] = (OpenDroneIdParser.MSG_TYPE_BASIC_ID shl 4).toByte()
        data[1] = 0x12
        serial.toByteArray(Charsets.US_ASCII)
            .copyInto(data, destinationOffset = 2, endIndex = serial.length.coerceAtMost(20))
        return data
    }

    private fun locationMessage(lat: Double, lon: Double): ByteArray {
        val data = ByteArray(25)
        data[0] = (OpenDroneIdParser.MSG_TYPE_LOCATION shl 4).toByte()
        data[2] = 45
        data[3] = 20
        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(5, (lat / 1e-7).toInt())
        buffer.putInt(9, (lon / 1e-7).toInt())
        buffer.putShort(13, ((120.0 + 1000.0) / 0.5).toInt().toShort())
        data[19] = 0x4A
        return data
    }

    private fun systemMessage(operatorLat: Double, operatorLon: Double): ByteArray {
        val data = ByteArray(25)
        data[0] = (OpenDroneIdParser.MSG_TYPE_SYSTEM shl 4).toByte()
        data[1] = (3 shl 2).toByte()
        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(2, (operatorLat / 1e-7).toInt())
        buffer.putInt(6, (operatorLon / 1e-7).toInt())
        buffer.putShort(10, 1)
        data[12] = 7
        buffer.putShort(13, ((120.0 + 1000.0) / 0.5).toInt().toShort())
        buffer.putShort(15, ((80.0 + 1000.0) / 0.5).toInt().toShort())
        data[17] = 0x34
        return data
    }

    private fun operatorIdMessage(operatorId: String): ByteArray {
        val data = ByteArray(25)
        data[0] = (OpenDroneIdParser.MSG_TYPE_OPERATOR_ID shl 4).toByte()
        operatorId.toByteArray(Charsets.US_ASCII)
            .copyInto(data, destinationOffset = 2, endIndex = operatorId.length.coerceAtMost(20))
        return data
    }

    private fun messagePack(vararg messages: ByteArray): ByteArray {
        val pack = ByteArray(3 + messages.size * 25)
        pack[0] = (OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK shl 4).toByte()
        pack[1] = 25
        pack[2] = messages.size.toByte()
        messages.forEachIndexed { index, message ->
            require(message.size == 25)
            message.copyInto(pack, destinationOffset = 3 + index * 25)
        }
        return pack
    }
}
