package com.friendorfoe.detection

import com.friendorfoe.domain.model.Drone
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Test

class BleRemoteIdFormationPipelineTest {

    @Test
    fun threeInterleavedC5BoardsPublishAllNinetySixFirstSightings() {
        val gate = BleRemoteIdFrameGate()
        val processor = BleRemoteIdPayloadProcessor()
        val startedAt = Instant.parse("2026-08-04T08:00:00Z")
        val boardAddresses = listOf(
            "F1:78:52:00:00:01",
            "9A:EE:2E:00:00:02",
            "9A:F4:26:00:00:03",
        )
        val boardSerials = listOf("F17852", "9AEE2E", "9AF426")
        val published = mutableListOf<Drone>()
        var callbackCount = 0
        var admittedCount = 0

        repeat(32) { slot ->
            val counter = (220 + slot) and 0xFF
            val messagesByBoard = boardAddresses.indices.map { board ->
                val pointIndex = slot * boardAddresses.size + board + 1
                val serial = "FOF-C5-${boardSerials[board]}-${pointIndex.toString().padStart(3, '0')}"
                val latitude = 37.30200 + pointIndex * 0.00001
                val longitude = -120.57650 + pointIndex * 0.00001
                listOf(
                    basicIdMessage(serial),
                    systemMessage(37.302787, -120.575859),
                    operatorIdMessage("FOF-FORMATION"),
                    locationMessage(latitude, longitude),
                )
            }

            repeat(4) { messageIndex ->
                repeat(5) { radioRepeat ->
                    boardAddresses.indices.forEach { board ->
                        val observationNanos =
                            slot * 500_000_000L +
                                messageIndex * 125_000_000L +
                                radioRepeat * 25_000_000L +
                                board * 1_000_000L
                        val serviceData =
                            byteArrayOf(0x0D, counter.toByte()) + messagesByBoard[board][messageIndex]
                        val descriptor =
                            requireNotNull(BleRemoteIdPayloadSelector.inspect(serviceData))
                        callbackCount++
                        val admission = gate.admit(
                            deviceAddress = boardAddresses[board],
                            serviceData = serviceData,
                            descriptor = descriptor,
                            observationTimestampNanos = observationNanos,
                        ) ?: return@forEach
                        admittedCount++
                        val selection =
                            requireNotNull(BleRemoteIdPayloadSelector.select(admission.serviceData))
                        processor.process(
                            deviceAddress = boardAddresses[board],
                            payload = selection.payload,
                            now = startedAt.plusNanos(observationNanos),
                            transportCounter = selection.transactionCounter,
                            observationTimestampNanos = observationNanos,
                        )?.let(published::add)
                    }
                }
            }
        }

        assertEquals(1_920, callbackCount)
        assertEquals(384, admittedCount)
        assertEquals(96, published.size)
        assertEquals(96, published.map(Drone::droneId).toSet().size)
        assertEquals(
            (1..96).toSet(),
            published.map { it.droneId.takeLast(3).toInt() }.toSet(),
        )
        assertEquals(
            96,
            published.map { it.position.latitude to it.position.longitude }.toSet().size,
        )
    }

    private fun basicIdMessage(serial: String): ByteArray = ByteArray(25).apply {
        this[0] = (OpenDroneIdParser.MSG_TYPE_BASIC_ID shl 4).toByte()
        this[1] = 0x12
        serial.toByteArray(Charsets.US_ASCII)
            .copyInto(this, destinationOffset = 2, endIndex = serial.length.coerceAtMost(20))
    }

    private fun locationMessage(latitude: Double, longitude: Double): ByteArray =
        ByteArray(25).also { data ->
            data[0] = (OpenDroneIdParser.MSG_TYPE_LOCATION shl 4).toByte()
            ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).apply {
                putInt(5, (latitude / 1e-7).toInt())
                putInt(9, (longitude / 1e-7).toInt())
                putShort(13, ((120.0 + 1000.0) / 0.5).toInt().toShort())
            }
        }

    private fun systemMessage(operatorLatitude: Double, operatorLongitude: Double): ByteArray =
        ByteArray(25).also { data ->
            data[0] = (OpenDroneIdParser.MSG_TYPE_SYSTEM shl 4).toByte()
            ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).apply {
                putInt(2, (operatorLatitude / 1e-7).toInt())
                putInt(6, (operatorLongitude / 1e-7).toInt())
            }
        }

    private fun operatorIdMessage(operatorId: String): ByteArray = ByteArray(25).apply {
        this[0] = (OpenDroneIdParser.MSG_TYPE_OPERATOR_ID shl 4).toByte()
        operatorId.toByteArray(Charsets.US_ASCII)
            .copyInto(this, destinationOffset = 2, endIndex = operatorId.length.coerceAtMost(20))
    }
}
