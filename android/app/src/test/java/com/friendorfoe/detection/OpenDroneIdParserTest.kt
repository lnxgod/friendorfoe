package com.friendorfoe.detection

import com.friendorfoe.domain.model.DetectionSource
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
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
        assertEquals(60.0, drone.estimatedDistanceMeters ?: -1.0, 6.0)
    }

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
        return data
    }
}
