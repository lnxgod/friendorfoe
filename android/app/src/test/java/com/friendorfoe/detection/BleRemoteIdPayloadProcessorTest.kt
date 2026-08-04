package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
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
    fun standaloneFormationCommitsOnLocationWithoutBorrowingPreviousAircraftLocation() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        processor.process(
            deviceAddress = address,
            payload = basicIdMessage("FOF-C5-A1B2C3-001"),
            now = Instant.parse("2026-07-04T12:00:00Z"),
            transportCounter = 10
        )
        processor.process(
            deviceAddress = address,
            payload = systemMessage(36.130940, -115.150640),
            now = Instant.parse("2026-07-04T12:00:00.025Z"),
            transportCounter = 10
        )
        processor.process(
            deviceAddress = address,
            payload = operatorIdMessage("FOF-FORMATION"),
            now = Instant.parse("2026-07-04T12:00:00.050Z"),
            transportCounter = 10
        )
        processor.process(
            deviceAddress = address,
            payload = locationMessage(36.13095, -115.15063),
            now = Instant.parse("2026-07-04T12:00:00.100Z"),
            transportCounter = 10
        )

        val nextBasic = processor.process(
            deviceAddress = address,
            payload = basicIdMessage("FOF-C5-A1B2C3-002"),
            now = Instant.parse("2026-07-04T12:00:00.200Z"),
            transportCounter = 11
        )
        processor.process(
            deviceAddress = address,
            payload = systemMessage(36.130940, -115.150640),
            now = Instant.parse("2026-07-04T12:00:00.225Z"),
            transportCounter = 11
        )
        processor.process(
            deviceAddress = address,
            payload = operatorIdMessage("FOF-FORMATION"),
            now = Instant.parse("2026-07-04T12:00:00.250Z"),
            transportCounter = 11
        )
        val nextLocation = processor.process(
            deviceAddress = address,
            payload = locationMessage(36.13100, -115.15050),
            now = Instant.parse("2026-07-04T12:00:00.300Z"),
            transportCounter = 11
        )

        assertNull(nextBasic)
        assertNotNull(nextLocation)
        requireNotNull(nextLocation)
        assertEquals("FOF-C5-A1B2C3-002", nextLocation.droneId)
        assertEquals(36.13100, nextLocation.position.latitude, 0.000001)
        assertEquals(-115.15050, nextLocation.position.longitude, 0.000001)
    }

    @Test
    fun standaloneFormationDropsLocationWhenItsBasicIdWasMissed() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(processor.process(
            deviceAddress = address,
            payload = basicIdMessage("FOF-C5-A1B2C3-001"),
            now = Instant.parse("2026-07-04T12:00:00Z"),
            transportCounter = 20
        ))
        assertNull(processor.process(
            deviceAddress = address,
            payload = systemMessage(36.130940, -115.150640),
            now = Instant.parse("2026-07-04T12:00:00.025Z"),
            transportCounter = 20
        ))
        assertNull(processor.process(
            deviceAddress = address,
            payload = operatorIdMessage("FOF-FORMATION"),
            now = Instant.parse("2026-07-04T12:00:00.050Z"),
            transportCounter = 20
        ))
        assertNotNull(processor.process(
            deviceAddress = address,
            payload = locationMessage(36.13095, -115.15063),
            now = Instant.parse("2026-07-04T12:00:00.100Z"),
            transportCounter = 20
        ))

        val orphanLocation = processor.process(
            deviceAddress = address,
            payload = locationMessage(36.13100, -115.15050),
            now = Instant.parse("2026-07-04T12:00:00.200Z"),
            transportCounter = 21
        )

        assertNull(orphanLocation)
    }

    @Test
    fun standaloneFormationAccumulatesFirmwareOrderAndCommitsCompletePointOnLocation() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(processor.process(
            deviceAddress = address,
            payload = basicIdMessage("FOF-C5-A1B2C3-003"),
            now = Instant.parse("2026-07-04T12:00:00Z"),
            transportCounter = 30
        ))
        assertNull(processor.process(
            deviceAddress = address,
            payload = systemMessage(36.130940, -115.150640),
            now = Instant.parse("2026-07-04T12:00:00.100Z"),
            transportCounter = 30
        ))
        assertNull(processor.process(
            deviceAddress = address,
            payload = operatorIdMessage("FOF-FORMATION"),
            now = Instant.parse("2026-07-04T12:00:00.200Z"),
            transportCounter = 30
        ))

        val committed = processor.process(
            deviceAddress = address,
            payload = locationMessage(36.13110, -115.15040),
            now = Instant.parse("2026-07-04T12:00:00.300Z"),
            transportCounter = 30
        )

        assertNotNull(committed)
        requireNotNull(committed)
        assertEquals("FOF-C5-A1B2C3-003", committed.droneId)
        assertEquals(36.13110, committed.position.latitude, 0.000001)
        assertEquals(-115.15040, committed.position.longitude, 0.000001)
        assertEquals(36.130940, committed.operatorLatitude ?: 0.0, 0.000001)
        assertEquals(-115.150640, committed.operatorLongitude ?: 0.0, 0.000001)
        assertEquals("FOF-FORMATION", committed.operatorId)
    }

    @Test
    fun missedLocationAndNextBasicCannotCommitNextPointUnderPreviousId() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(formationFrame(processor, address,
            basicIdMessage("FOF-C5-A1B2C3-010"),
            Instant.parse("2026-07-04T12:00:00Z"), 40))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.100Z"), 40))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.200Z"), 40))

        // Point 010 Location and point 011 Basic are both missed. The next
        // System is out of phase and must close 010 rather than allowing the
        // following Location to publish 011's coordinates under ID 010.
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.500Z"), 41))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.600Z"), 41))
        assertNull(formationFrame(processor, address,
            locationMessage(36.13120, -115.15030),
            Instant.parse("2026-07-04T12:00:00.700Z"), 41))
    }

    @Test
    fun delayedPreviousLocationCannotCrossCommitAfterNewPointIsReady() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(formationFrame(processor, address,
            basicIdMessage("FOF-C5-A1B2C3-020"),
            Instant.parse("2026-07-04T12:00:00Z"), 50))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.100Z"), 50))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.200Z"), 50))
        assertNull(formationFrame(processor, address,
            basicIdMessage("FOF-C5-A1B2C3-021"),
            Instant.parse("2026-07-04T12:00:00.400Z"), 51))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.500Z"), 51))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.600Z"), 51))

        // A's delayed Location has A's transport counter and is discarded
        // even though B is currently waiting for a Location message.
        assertNull(formationFrame(processor, address,
            locationMessage(36.13130, -115.15020),
            Instant.parse("2026-07-04T12:00:00.650Z"), 50))
        val committed = formationFrame(processor, address,
            locationMessage(36.13140, -115.15010),
            Instant.parse("2026-07-04T12:00:00.700Z"), 51)
        assertNotNull(committed)
        requireNotNull(committed)
        assertEquals("FOF-C5-A1B2C3-021", committed.droneId)
        assertEquals(36.13140, committed.position.latitude, 0.000001)
        assertEquals(-115.15010, committed.position.longitude, 0.000001)
    }

    @Test
    fun sameTypeFrameOutsideDuplicateWindowCannotBridgeAMissedBasic() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(formationFrame(processor, address,
            basicIdMessage("FOF-C5-A1B2C3-025"),
            Instant.parse("2026-07-04T12:00:00Z"), 60))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.100Z"), 60))

        // Operator, Location, and the next Basic are missed. A System from the
        // next point is 400 ms newer, not a duplicate of 025's System.
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.500Z"), 61))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.600Z"), 61))
        assertNull(formationFrame(processor, address,
            locationMessage(36.13145, -115.15005),
            Instant.parse("2026-07-04T12:00:00.700Z"), 61))
    }

    @Test
    fun duplicateFirmwareFramesPreserveTransactionProgress() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"
        val serial = "FOF-C5-A1B2C3-030"

        repeat(2) { index ->
            assertNull(formationFrame(processor, address, basicIdMessage(serial),
                Instant.parse("2026-07-04T12:00:00.${index}00Z"), 70))
        }
        repeat(2) { index ->
            assertNull(formationFrame(processor, address,
                systemMessage(36.130940, -115.150640),
                Instant.parse("2026-07-04T12:00:00.${index + 2}00Z"), 70))
        }
        repeat(2) { index ->
            assertNull(formationFrame(processor, address,
                operatorIdMessage("FOF-FORMATION"),
                Instant.parse("2026-07-04T12:00:00.${index + 4}00Z"), 70))
        }

        val committed = formationFrame(processor, address,
            locationMessage(36.13150, -115.15000),
            Instant.parse("2026-07-04T12:00:00.600Z"), 70)
        assertNotNull(committed)
        requireNotNull(committed)
        assertEquals(serial, committed.droneId)
        assertEquals(36.13150, committed.position.latitude, 0.000001)
        assertEquals(-115.15000, committed.position.longitude, 0.000001)
    }

    @Test
    fun blankBasicCannotReusePreviousFormationIdentity() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"

        assertNull(formationFrame(processor, address,
            basicIdMessage("FOF-C5-A1B2C3-040"),
            Instant.parse("2026-07-04T12:00:00Z"), 80))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.100Z"), 80))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.200Z"), 80))
        assertNotNull(formationFrame(processor, address,
            locationMessage(36.13150, -115.15000),
            Instant.parse("2026-07-04T12:00:00.300Z"), 80))

        assertNull(formationFrame(processor, address, ByteArray(25),
            Instant.parse("2026-07-04T12:00:00.400Z"), 81))
        assertNull(formationFrame(processor, address,
            systemMessage(36.130940, -115.150640),
            Instant.parse("2026-07-04T12:00:00.500Z"), 81))
        assertNull(formationFrame(processor, address,
            operatorIdMessage("FOF-FORMATION"),
            Instant.parse("2026-07-04T12:00:00.600Z"), 81))
        assertNull(formationFrame(processor, address,
            locationMessage(36.13160, -115.14990),
            Instant.parse("2026-07-04T12:00:00.700Z"), 81))
    }

    @Test
    fun fullDefconCarouselPublishesNinetySixUniqueStationaryPoints() {
        val processor = BleRemoteIdPayloadProcessor()
        val address = "AA:BB:CC:DD:EE:FF"
        val startedAt = Instant.parse("2026-07-04T12:00:00Z")

        val published = (1..96).map { pointIndex ->
            val pointStart = startedAt.plusMillis((pointIndex - 1) * 400L)
            val counter = (219 + pointIndex) and 0xFF
            val serial = "FOF-C5-A1B2C3-${pointIndex.toString().padStart(3, '0')}"
            val latitude = 36.13000 + pointIndex * 0.00001
            val longitude = -115.15000 + pointIndex * 0.00001

            assertNull(formationFrame(processor, address, basicIdMessage(serial),
                pointStart, counter))
            assertNull(formationFrame(processor, address,
                systemMessage(36.13094, -115.15064),
                pointStart.plusMillis(100), counter))
            assertNull(formationFrame(processor, address,
                operatorIdMessage("FOF-FORMATION"),
                pointStart.plusMillis(200), counter))
            requireNotNull(formationFrame(processor, address,
                locationMessage(latitude, longitude),
                pointStart.plusMillis(300), counter))
        }

        assertEquals(96, published.map { it.droneId }.toSet().size)
        assertEquals(96, published.map {
            it.position.latitude to it.position.longitude
        }.toSet().size)
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

    private fun formationFrame(
        processor: BleRemoteIdPayloadProcessor,
        address: String,
        payload: ByteArray,
        now: Instant,
        counter: Int
    ) = processor.process(
        deviceAddress = address,
        payload = payload,
        now = now,
        transportCounter = counter
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
