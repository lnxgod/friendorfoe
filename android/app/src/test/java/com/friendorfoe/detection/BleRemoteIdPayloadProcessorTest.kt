package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant

class BleRemoteIdPayloadProcessorTest {

    @Test
    fun continuesAccumulatingStandaloneMessagesByTransmitter() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        val basicOnly = processor.process(
            deviceAddress = address,
            payload = basicIdMessage("SERIAL123"),
            now = Instant.parse("2026-07-04T12:00:00Z")
        )
        val withLocation = processor.process(
            deviceAddress = address,
            payload = locationMessage(37.7749, -122.4194),
            now = Instant.parse("2026-07-04T12:00:01Z")
        )

        assertNotNull(basicOnly)
        assertNotNull(withLocation)
        requireNotNull(withLocation)
        assertEquals("SERIAL123", withLocation.droneId)
        assertEquals(37.7749, withLocation.position.latitude, 0.000001)
        assertEquals(-122.4194, withLocation.position.longitude, 0.000001)
    }

    @Test
    fun isolatesConsecutiveAtomicPacksFromOneTransmitterByBasicId() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        val first = processor.process(
            deviceAddress = address,
            payload = formationPack(
                serial = "FOF-C5-A1B2C3-001",
                latitude = 36.13094,
                longitude = -115.15064,
                operatorLatitude = 36.13100,
                operatorLongitude = -115.15050,
                operatorId = "OPERATOR-A"
            ),
            now = Instant.parse("2026-07-04T12:00:00Z"),
            signalStrengthDbm = -61,
            estimatedDistanceMeters = 3.5
        )
        val second = processor.process(
            deviceAddress = address,
            payload = formationPack(
                serial = "FOF-C5-A1B2C3-002",
                latitude = 36.13120,
                longitude = -115.15010,
                operatorLatitude = 36.13200,
                operatorLongitude = -115.14990,
                operatorId = "OPERATOR-B"
            ),
            now = Instant.parse("2026-07-04T12:00:01Z"),
            signalStrengthDbm = -63,
            estimatedDistanceMeters = 4.0
        )

        assertNotNull(first)
        assertNotNull(second)
        requireNotNull(first)
        requireNotNull(second)
        assertEquals("rid_FOF-C5-A1B2C3-001", first.id)
        assertEquals("FOF-C5-A1B2C3-001", first.droneId)
        assertEquals(36.13094, first.position.latitude, 0.000001)
        assertEquals(-115.15064, first.position.longitude, 0.000001)
        assertEquals(36.13100, first.operatorLatitude ?: 0.0, 0.000001)
        assertEquals("OPERATOR-A", first.operatorId)
        assertEquals("rid_FOF-C5-A1B2C3-002", second.id)
        assertEquals("FOF-C5-A1B2C3-002", second.droneId)
        assertEquals(36.13120, second.position.latitude, 0.000001)
        assertEquals(-115.15010, second.position.longitude, 0.000001)
        assertEquals(36.13200, second.operatorLatitude ?: 0.0, 0.000001)
        assertEquals("OPERATOR-B", second.operatorId)
    }

    @Test
    fun freshPackStateDoesNotBorrowLocationFromPreviousSerial() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"
        processor.process(
            deviceAddress = address,
            payload = formationPack(
                serial = "FOF-C5-A1B2C3-001",
                latitude = 36.13094,
                longitude = -115.15064,
                operatorLatitude = 36.13100,
                operatorLongitude = -115.15050,
                operatorId = "OPERATOR-A"
            ),
            now = Instant.parse("2026-07-04T12:00:00Z")
        )

        val basicOnly = processor.process(
            deviceAddress = address,
            payload = messagePack(basicIdMessage("FOF-C5-A1B2C3-002")),
            now = Instant.parse("2026-07-04T12:00:01Z")
        )

        assertNotNull(basicOnly)
        requireNotNull(basicOnly)
        assertEquals("FOF-C5-A1B2C3-002", basicOnly.droneId)
        assertEquals(0.0, basicOnly.position.latitude, 0.0)
        assertEquals(0.0, basicOnly.position.longitude, 0.0)
    }

    private fun formationPack(
        serial: String,
        latitude: Double,
        longitude: Double,
        operatorLatitude: Double,
        operatorLongitude: Double,
        operatorId: String
    ): ByteArray = messagePack(
        basicIdMessage(serial),
        locationMessage(latitude, longitude),
        systemMessage(operatorLatitude, operatorLongitude),
        operatorIdMessage(operatorId)
    )

    private fun basicIdMessage(serial: String): ByteArray = ByteArray(25).apply {
        this[0] = (OpenDroneIdParser.MSG_TYPE_BASIC_ID shl 4).toByte()
        this[1] = 0x12
        serial.toByteArray(Charsets.US_ASCII)
            .copyInto(this, destinationOffset = 2, endIndex = serial.length.coerceAtMost(20))
    }

    private fun locationMessage(latitude: Double, longitude: Double): ByteArray =
        ByteArray(25).also { data ->
            data[0] = (OpenDroneIdParser.MSG_TYPE_LOCATION shl 4).toByte()
            val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(5, (latitude / 1e-7).toInt())
            buffer.putInt(9, (longitude / 1e-7).toInt())
            buffer.putShort(13, ((120.0 + 1000.0) / 0.5).toInt().toShort())
        }

    private fun systemMessage(operatorLatitude: Double, operatorLongitude: Double): ByteArray =
        ByteArray(25).also { data ->
            data[0] = (OpenDroneIdParser.MSG_TYPE_SYSTEM shl 4).toByte()
            val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(2, (operatorLatitude / 1e-7).toInt())
            buffer.putInt(6, (operatorLongitude / 1e-7).toInt())
        }

    private fun operatorIdMessage(operatorId: String): ByteArray = ByteArray(25).apply {
        this[0] = (OpenDroneIdParser.MSG_TYPE_OPERATOR_ID shl 4).toByte()
        operatorId.toByteArray(Charsets.US_ASCII)
            .copyInto(this, destinationOffset = 2, endIndex = operatorId.length.coerceAtMost(20))
    }

    private fun messagePack(vararg messages: ByteArray): ByteArray {
        return ByteArray(3 + messages.size * 25).also { pack ->
            pack[0] = (OpenDroneIdParser.MSG_TYPE_MESSAGE_PACK shl 4).toByte()
            pack[1] = 25
            pack[2] = messages.size.toByte()
            messages.forEachIndexed { index, message ->
                require(message.size == 25)
                message.copyInto(pack, destinationOffset = 3 + index * 25)
            }
        }
    }
}
