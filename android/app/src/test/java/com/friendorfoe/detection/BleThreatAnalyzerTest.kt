package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BleThreatAnalyzerTest {

    @Test
    fun `twelve rotating Swift Pair addresses in eight seconds emit one flood`() {
        val analyzer = BleThreatAnalyzer()
        val signals = (0 until 12).flatMap { index ->
            listOf(index * 2L, index * 2L + 1L).flatMap { packet ->
                analyzer.observe(prompt(index, packet * 300L))
            }
        }

        val flood = signals.filterIsInstance<BleThreatSignal.PairingSpam>().single()
        assertEquals(12, flood.uniqueMacs)
        assertEquals(24, flood.observationCount)
        assertEquals(setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR), flood.families)
    }

    @Test
    fun `scan overlap duplicate does not increase observation count`() {
        val analyzer = BleThreatAnalyzer()

        analyzer.observe(prompt(1, 1_000))
        analyzer.observe(prompt(1, 1_100))

        val snapshot = analyzer.debugSnapshot()
        assertEquals(1, snapshot.deduplicatedObservations)
    }

    @Test
    fun `prompt bookkeeping removes expired signature buckets and dedupe metadata`() {
        val analyzer = BleThreatAnalyzer()

        repeat(300) { index ->
            analyzer.observe(prompt(index, index * 300L, structuralHash = index))
        }

        val snapshot = analyzer.debugSnapshot()
        assertEquals(256, snapshot.firstSeenIdentities)
        assertEquals(256, snapshot.signatureBuckets)
        assertEquals(1, snapshot.dedupeEntries)
    }

    @Test
    fun `prompt dedupe metadata stays bounded when prompt capacity evicts samples`() {
        val analyzer = BleThreatAnalyzer()

        repeat(300) { index ->
            analyzer.observe(prompt(index, 0, structuralHash = index))
        }

        assertEquals(256, analyzer.debugSnapshot().dedupeEntries)
    }

    @Test
    fun `varied crowd does not alert when RSSI spread exceeds twenty dB`() {
        val analyzer = BleThreatAnalyzer()
        val signals = promptBurst(rssiFor = { index, _ -> if (index % 2 == 0) -40 else -64 })
            .flatMap(analyzer::observe)

        assertTrue(signals.filterIsInstance<BleThreatSignal.PairingSpam>().isEmpty())
    }

    @Test
    fun `stable addresses do not satisfy seventy five percent churn`() {
        val analyzer = BleThreatAnalyzer()
        (0 until 12).forEach { index -> analyzer.observe(prompt(index, index * 300L)) }

        val signals = promptBurst(startAtMs = 9_000).flatMap(analyzer::observe)

        assertTrue(signals.filterIsInstance<BleThreatSignal.PairingSpam>().isEmpty())
    }

    @Test
    fun `isolated Fast Pair and generic Apple traffic do not alert`() {
        val analyzer = BleThreatAnalyzer()
        val signals = buildList {
            repeat(11) { index ->
                add(prompt(index, index * 300L, BlePromptFamily.GOOGLE_FAST_PAIR))
            }
            repeat(11) { index ->
                add(genericApple(index + 20, (index + 11L) * 300L))
            }
        }.flatMap(analyzer::observe)

        assertTrue(signals.filterIsInstance<BleThreatSignal.PairingSpam>().isEmpty())
    }

    @Test
    fun `mixed Apple Fast Pair and Swift Pair burst alerts once`() {
        val analyzer = BleThreatAnalyzer()
        val families = listOf(
            BlePromptFamily.APPLE_CONTINUITY,
            BlePromptFamily.GOOGLE_FAST_PAIR,
            BlePromptFamily.MICROSOFT_SWIFT_PAIR,
        )
        val signals = promptBurst { index, _ -> families[index % families.size] }
            .flatMap(analyzer::observe)

        val floods = signals.filterIsInstance<BleThreatSignal.PairingSpam>()
        assertEquals(1, floods.size)
        assertEquals(families.toSet(), floods.single().families)
    }

    @Test
    fun `sustained flood respects sixty second cooldown`() {
        val analyzer = BleThreatAnalyzer()
        val signals = (0 until 4).flatMap { burst ->
            promptBurst(startAtMs = burst * 8_000L).flatMap(analyzer::observe)
        }

        assertEquals(1, signals.filterIsInstance<BleThreatSignal.PairingSpam>().size)
    }

    @Test
    fun `pairing spam clears after twenty quiet seconds`() {
        val analyzer = BleThreatAnalyzer()
        val signals = buildList {
            addAll(promptBurst().flatMap(analyzer::observe))
            addAll(promptBurst(startAtMs = 27_000, indexOffset = 20).flatMap(analyzer::observe))
        }

        assertEquals(2, signals.filterIsInstance<BleThreatSignal.PairingSpam>().size)
    }

    @Test
    fun `pairing spam clears at exactly twenty quiet seconds`() {
        val analyzer = BleThreatAnalyzer()
        val signals = buildList {
            addAll(promptBurst().flatMap(analyzer::observe))
            addAll(promptBurst(startAtMs = 26_900, indexOffset = 20).flatMap(analyzer::observe))
        }

        assertEquals(2, signals.filterIsInstance<BleThreatSignal.PairingSpam>().size)
    }

    @Test
    fun `persistent close sparse FFE0 device emits possible serial skimmer`() {
        val analyzer = BleThreatAnalyzer()

        analyzer.observe(serial(0))
        analyzer.observe(serial(2_500))
        val signal = analyzer.observe(serial(5_100))
            .filterIsInstance<BleThreatSignal.SerialSkimmer>()
            .single()

        assertEquals(0xFFE0, signal.serialServiceUuid)
        assertTrue(signal.evidence.contains(BleSerialEvidence.PERSISTENT))
    }

    @Test
    fun `serial signal carries strongest observed RSSI`() {
        val analyzer = BleThreatAnalyzer()

        analyzer.observe(serial(0, rssi = -61))
        analyzer.observe(serial(2_500, rssi = -70))
        val signal = analyzer.observe(serial(5_100, rssi = -66))
            .filterIsInstance<BleThreatSignal.SerialSkimmer>()
            .single()

        assertEquals(-61, signal.strongestRssi)
    }

    @Test
    fun `simultaneous prompt and serial remain independently observable`() {
        val analyzer = BleThreatAnalyzer()
        val burst = promptBurst()
        val collisionMac = burst.last().mac
        analyzer.observe(serial(0).copy(mac = collisionMac))
        analyzer.observe(serial(2_500).copy(mac = collisionMac))
        burst.dropLast(1).forEach(analyzer::observe)

        val collisionSignals = analyzer.observe(
            burst.last().copy(
                serviceUuids16 = setOf(0xFFE0),
                localName = "BT",
                connectable = true,
            )
        )

        assertTrue(collisionSignals.single() is BleThreatSignal.PairingSpam)
        val nextSerialPacket = serial(7_200).copy(mac = collisionMac)
        assertTrue(analyzer.observe(nextSerialPacket).single() is BleThreatSignal.SerialSkimmer)
    }

    @Test
    fun `persistent sparse FFE0 candidate with exactly two supporting signals alerts`() {
        val analyzer = BleThreatAnalyzer()

        analyzer.observe(serial(0, name = null, connectable = false))
        analyzer.observe(serial(2_500, name = null, connectable = false))
        val signal = analyzer.observe(serial(5_100, name = null, connectable = false))
            .filterIsInstance<BleThreatSignal.SerialSkimmer>()
            .single()

        assertEquals(
            setOf(
                BleSerialEvidence.SERIAL_UUID,
                BleSerialEvidence.SPARSE_PROFILE,
                BleSerialEvidence.PERSISTENT,
                BleSerialEvidence.CLOSE,
                BleSerialEvidence.UNTRUSTED,
            ),
            signal.evidence,
        )
    }

    @Test
    fun `FFE0 alone never alerts`() {
        val analyzer = BleThreatAnalyzer()
        val signals = listOf(0L, 2_500L, 5_100L)
            .flatMap { analyzer.observe(serial(it, name = null, rssi = -90, connectable = false)) }

        assertTrue(signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().isEmpty())
    }

    @Test
    fun `weak nonpersistent UART device never alerts`() {
        val analyzer = BleThreatAnalyzer()
        val signals = listOf(0L, 2_500L)
            .flatMap { analyzer.observe(serial(it, rssi = -86)) }

        assertTrue(signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().isEmpty())
    }

    @Test
    fun `trusted product suppresses serial heuristic`() {
        val analyzer = BleThreatAnalyzer()
        val signals = listOf(0L, 2_500L, 5_100L)
            .flatMap { analyzer.observe(serial(it, trusted = true)) }

        assertTrue(signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().isEmpty())
    }

    @Test
    fun `multi service device suppresses sparse profile evidence`() {
        val analyzer = BleThreatAnalyzer()
        val signals = listOf(0L, 2_500L, 5_100L)
            .flatMap { analyzer.observe(serial(it, services = setOf(0xFFE0, 0xFEAA))) }

        assertTrue(signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().isEmpty())
    }

    @Test
    fun `PKOC identity suppresses FFF0 heuristic`() {
        val analyzer = BleThreatAnalyzer()
        val signals = listOf(0L, 2_500L, 5_100L)
            .flatMap { analyzer.observe(serial(it, services = setOf(0xFFF0), name = "PKOC")) }

        assertTrue(signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().isEmpty())
    }

    @Test
    fun `prompt quiet clearing preserves an active serial track`() {
        val analyzer = BleThreatAnalyzer()
        analyzer.observe(prompt(1, 0))
        analyzer.observe(serial(0))
        analyzer.observe(serial(2_500))
        analyzer.observe(prompt(2, 21_000))

        val signals = analyzer.observe(serial(22_000))

        assertEquals(1, signals.filterIsInstance<BleThreatSignal.SerialSkimmer>().size)
    }

    @Test
    fun `prompt family only maps decoded prompt evidence`() {
        assertEquals(
            BlePromptFamily.MICROSOFT_SWIFT_PAIR,
            advertisement(microsoft = MicrosoftSwiftPairDecoder.SwiftPair(0x03, 0x00, null)).promptFamily(),
        )
        assertEquals(
            BlePromptFamily.GOOGLE_FAST_PAIR,
            advertisement(serviceUuids16 = listOf(0xFE2C)).promptFamily(),
        )
        assertEquals(
            BlePromptFamily.APPLE_CONTINUITY,
            advertisement(
                companyId = BleSignatures.CID_APPLE,
                apple = AppleContinuityDecoder.AppleContinuity(
                    subType = 0x10,
                    deviceType = AppleContinuityDecoder.AppleDeviceType.APPLE_GENERIC,
                    authTag = null,
                    activity = null,
                    flagsByte = null,
                    iosVersionNibble = null,
                    nearbyActionSubType = null,
                ),
            ).promptFamily(),
        )
        assertNull(advertisement(companyId = BleSignatures.CID_APPLE).promptFamily())
        assertNull(advertisement(companyId = BleSignatures.CID_MICROSOFT).promptFamily())
    }

    @Test
    fun `advertisement connectability defaults to false`() {
        assertFalse(advertisement().connectable)
    }

    private fun promptBurst(
        startAtMs: Long = 0,
        indexOffset: Int = 0,
        rssiFor: (index: Int, packet: Long) -> Int = { _, _ -> -48 },
        familyFor: (index: Int, packet: Long) -> BlePromptFamily = {
                _, _ -> BlePromptFamily.MICROSOFT_SWIFT_PAIR
        },
    ): List<BleThreatObservation> = (0 until 12).flatMap { index ->
        listOf(index * 2L, index * 2L + 1L).map { packet ->
            prompt(
                index = index + indexOffset,
                atMs = startAtMs + packet * 300L,
                family = familyFor(index, packet),
                rssi = rssiFor(index, packet),
            )
        }
    }

    private fun prompt(
        index: Int,
        atMs: Long,
        family: BlePromptFamily = BlePromptFamily.MICROSOFT_SWIFT_PAIR,
        rssi: Int = -48,
        structuralHash: Int = 0x1234,
    ) = BleThreatObservation(
        mac = "02:00:00:00:00:${index.toString(16).padStart(2, '0')}",
        observedAtMs = atMs,
        rssi = rssi,
        connectable = false,
        structuralHash = structuralHash,
        promptFamily = family,
        serviceUuids16 = emptySet(),
        localName = null,
        companyId = 0x0006,
        trustedIdentity = false,
    )

    private fun genericApple(index: Int, atMs: Long): BleThreatObservation = prompt(index, atMs).copy(
        promptFamily = null,
        companyId = BleSignatures.CID_APPLE,
    )

    private fun serial(
        atMs: Long,
        services: Set<Int> = setOf(0xFFE0),
        name: String? = "BT",
        rssi: Int = -62,
        connectable: Boolean = true,
        trusted: Boolean = false,
    ) = BleThreatObservation(
        mac = "C0:98:E5:00:00:01",
        observedAtMs = atMs,
        rssi = rssi,
        connectable = connectable,
        structuralHash = 0xFFE0,
        promptFamily = null,
        serviceUuids16 = services,
        localName = name,
        companyId = null,
        trustedIdentity = trusted,
    )

    private fun advertisement(
        companyId: Int? = null,
        serviceUuids16: List<Int> = emptyList(),
        apple: AppleContinuityDecoder.AppleContinuity? = null,
        microsoft: MicrosoftSwiftPairDecoder.SwiftPair? = null,
    ) = BleAdvertisement(
        mac = "02:00:00:00:00:01",
        rssi = -48,
        totalLength = 0,
        adTypes = emptyList(),
        payloadStructHash = 0x1234,
        companyId = companyId,
        serviceUuids16 = serviceUuids16,
        apple = apple,
        microsoft = microsoft,
    )
}
