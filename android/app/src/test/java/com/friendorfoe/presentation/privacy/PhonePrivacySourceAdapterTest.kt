package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.repository.LocalDetectionPermissions
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.GlassesScanEvent
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.UltrasonicDetector
import com.friendorfoe.detection.UltrasonicScanEvent
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PhonePrivacySourceAdapterTest {

    @Test
    fun enabledStartupDoesNotRestampItsLoadingDeadline() = runTest {
        val clock = FakeClock(elapsed = 1_000L)
        val adapter = adapter(
            settings = MutableStateFlow(
                DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
            ),
            permissions = MutableStateFlow(
                LocalDetectionPermissions.None.copy(bluetoothScan = true),
            ),
            clock = clock,
        )
        clock.elapsed = 5_000L

        runCurrent()

        assertEquals(SourceHealthState.LOADING, adapter.bleSnapshot().health.state)
        assertEquals(1_000L, adapter.bleSnapshot().emittedAtElapsedMs)
    }

    @Test
    fun rssiSampleStreamUsesTheMappedObservationKeyAndCapturedBearing() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 1)
        val expectedKey = PrivacyFindingKey(
            PrivacySourceKind.PHONE_BLE,
            "observation:fp:meta-one",
        )
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { events },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = BleTracker(),
            clock = FakeClock(elapsed = 2_000L),
            scope = backgroundScope,
            compassBearing = { 123f },
        )
        val received = mutableListOf<RssiSample>()
        backgroundScope.launch {
            adapter.samplesFor(expectedKey).take(1).collect(received::add)
        }
        runCurrent()

        events.emit(GlassesScanEvent.Observation(glasses("fp:meta-one", "AA:BB")))
        runCurrent()

        assertEquals(
            listOf(RssiSample(expectedKey, -52, 123f, observedAtElapsedMs = 2_000L)),
            received,
        )
    }

    @Test
    fun rssiSampleStreamWaitsForARealOrientationReading() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 1)
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { events },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = BleTracker(),
            clock = FakeClock(elapsed = 2_000L),
            scope = backgroundScope,
            compassBearing = { null },
        )
        val received = mutableListOf<RssiSample>()
        backgroundScope.launch {
            adapter.samplesFor(
                PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "observation:fp:meta-one"),
            ).collect(received::add)
        }
        runCurrent()

        events.emit(GlassesScanEvent.Observation(glasses("fp:meta-one", "AA:BB")))
        runCurrent()

        assertTrue(received.isEmpty())
    }

    @Test
    fun disabledPhoneSourcesResolveAsTwoPausedSnapshotsWithoutStartingCollectors() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val permissions = MutableStateFlow(LocalDetectionPermissions.None)
        var bleStarts = 0
        var ultrasonicStarts = 0
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { flow { bleStarts += 1 } },
            ultrasonicEvents = { flow { ultrasonicStarts += 1 } },
        )
        runCurrent()

        assertEquals(
            listOf(PrivacySourceKind.PHONE_BLE, PrivacySourceKind.PHONE_ULTRASONIC),
            adapter.snapshots.value.map { it.health.source },
        )
        assertTrue(adapter.snapshots.value.all { it.health.state == SourceHealthState.PAUSED })
        assertEquals(0, bleStarts)
        assertEquals(0, ultrasonicStarts)
    }

    @Test
    fun permissionGrantStartsOneBleCollectorAndMultipleSnapshotCollectorsDoNotRestartIt() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None)
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 4)
        var bleStarts = 0
        val tracker = BleTracker()
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleTracker = tracker,
            bleEvents = {
                flow {
                    bleStarts += 1
                    events.collect { emit(it) }
                }
            },
        )
        runCurrent()

        assertEquals(SourceHealthState.PERMISSION_BLOCKED, adapter.bleSnapshot().health.state)
        assertEquals(0, bleStarts)

        permissions.value = permissions.value.copy(bluetoothScan = true)
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(SourceHealthState.LOADING, adapter.bleSnapshot().health.state)

        val firstCollector = backgroundScope.launch { adapter.snapshots.collect {} }
        val secondCollector = backgroundScope.launch { adapter.snapshots.collect {} }
        runCurrent()
        assertEquals(1, bleStarts)

        events.emit(GlassesScanEvent.Ready)
        runCurrent()
        assertEquals(SourceHealthState.LIVE, adapter.bleSnapshot().health.state)
        assertEquals(1_000L, adapter.bleSnapshot().health.lastSuccessElapsedMs)
        assertTrue(adapter.bleSnapshot().findings.isEmpty())

        events.emit(GlassesScanEvent.Observation(glasses("fp:meta-one", "AA:BB")))
        runCurrent()
        assertEquals(SourceHealthState.LIVE, adapter.bleSnapshot().health.state)
        assertEquals("fp:meta-one", adapter.bleSnapshot().findings.single().stableSourceId)
        assertEquals(1, tracker.getDevice("AA:BB")?.sightingCount)
        firstCollector.cancel()
        secondCollector.cancel()
    }

    @Test
    fun failureAndPauseRetainBleRowsWithoutRejuvenatingObservationTime() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 4)
        val clock = FakeClock(elapsed = 10_000L, wall = 100_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            bleEvents = { events },
        )
        runCurrent()

        events.emit(GlassesScanEvent.Observation(glasses("fp:one", "AA:BB")))
        runCurrent()
        val observedAt = adapter.bleSnapshot().findings.single().lastObservedElapsedMs
        assertEquals(10_000L, observedAt)

        clock.elapsed = 40_000L
        clock.wall = 900_000L
        events.emit(GlassesScanEvent.Failure("scanner failed"))
        runCurrent()
        assertEquals(SourceHealthState.FAILED, adapter.bleSnapshot().health.state)
        assertEquals(observedAt, adapter.bleSnapshot().findings.single().lastObservedElapsedMs)

        settings.value = settings.value.copy(phonePrivacyScanEnabled = false)
        runCurrent()
        assertEquals(SourceHealthState.PAUSED, adapter.bleSnapshot().health.state)
        assertEquals(observedAt, adapter.bleSnapshot().findings.single().lastObservedElapsedMs)

        settings.value = settings.value.copy(phonePrivacyScanEnabled = true)
        runCurrent()
        assertEquals(SourceHealthState.STALE, adapter.bleSnapshot().health.state)
        assertEquals(observedAt, adapter.bleSnapshot().findings.single().lastObservedElapsedMs)
    }

    @Test
    fun backendOnlyPausesEnabledPhoneCollectorsAndCancelsTheirSourceFlows() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                ultrasonicEnabled = true,
            ),
        )
        val permissions = MutableStateFlow(
            LocalDetectionPermissions.None.copy(bluetoothScan = true, audioCapture = true),
        )
        var bleCancellations = 0
        var ultrasonicCancellations = 0
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flow {
                    try {
                        kotlinx.coroutines.awaitCancellation()
                    } finally {
                        bleCancellations += 1
                    }
                }
            },
            ultrasonicEvents = {
                flow {
                    try {
                        kotlinx.coroutines.awaitCancellation()
                    } finally {
                        ultrasonicCancellations += 1
                    }
                }
            },
        )
        runCurrent()

        settings.value = settings.value.copy(backendOnlyMode = true)
        runCurrent()

        assertTrue(adapter.snapshots.value.all { it.health.state == SourceHealthState.PAUSED })
        assertEquals(1, bleCancellations)
        assertEquals(1, ultrasonicCancellations)
    }

    @Test
    fun permissionRestartCancelsTheOldBleFlowBeforeStartingItsReplacement() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val lifecycle = mutableListOf<String>()
        var current = 0
        var maximum = 0
        var generation = 0
        adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flow {
                    val ownGeneration = ++generation
                    current += 1
                    maximum = maxOf(maximum, current)
                    lifecycle += "start:$ownGeneration"
                    try {
                        kotlinx.coroutines.awaitCancellation()
                    } finally {
                        current -= 1
                        lifecycle += "cancel:$ownGeneration"
                    }
                }
            },
        )
        runCurrent()

        permissions.value = permissions.value.copy(bluetoothScan = false)
        runCurrent()
        permissions.value = permissions.value.copy(bluetoothScan = true)
        runCurrent()

        assertEquals(1, maximum)
        assertEquals(listOf("start:1", "cancel:1", "start:2"), lifecycle)
    }

    @Test
    fun recoveringBleRestartsOnlyTheBleCollector() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                ultrasonicEnabled = true,
            ),
        )
        val permissions = MutableStateFlow(
            LocalDetectionPermissions.None.copy(bluetoothScan = true, audioCapture = true),
        )
        var bleStarts = 0
        var ultrasonicStarts = 0
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flow {
                    bleStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
            ultrasonicEvents = {
                flow {
                    ultrasonicStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
        )
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(1, ultrasonicStarts)

        assertEquals(
            PrivacyRecoveryResult.Recovered(PrivacySourceKind.PHONE_BLE),
            adapter.recover(PrivacySourceKind.PHONE_BLE),
        )
        runCurrent()

        assertEquals(2, bleStarts)
        assertEquals(1, ultrasonicStarts)
    }

    @Test
    fun ultrasonicSettingAndPermissionChangesDoNotRestartBleScanning() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                ultrasonicEnabled = true,
            ),
        )
        val permissions = MutableStateFlow(
            LocalDetectionPermissions.None.copy(bluetoothScan = true, audioCapture = true),
        )
        var bleStarts = 0
        var ultrasonicStarts = 0
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flow {
                    bleStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
            ultrasonicEvents = {
                flow {
                    ultrasonicStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
        )
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(1, ultrasonicStarts)

        settings.value = settings.value.copy(ultrasonicEnabled = false)
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(1, ultrasonicStarts)

        settings.value = settings.value.copy(ultrasonicEnabled = true)
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(2, ultrasonicStarts)

        permissions.value = permissions.value.copy(audioCapture = false)
        runCurrent()
        assertEquals(1, bleStarts)
        assertEquals(2, ultrasonicStarts)
        assertEquals(SourceHealthState.PERMISSION_BLOCKED, adapter.ultrasonicSnapshot().health.state)
    }

    @Test
    fun unrelatedSettingsAndStalkerToggleDoNotRestartPhoneCollectors() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                ultrasonicEnabled = true,
            ),
        )
        val permissions = MutableStateFlow(
            LocalDetectionPermissions.None.copy(bluetoothScan = true, audioCapture = true),
        )
        var bleStarts = 0
        var ultrasonicStarts = 0
        adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flow {
                    bleStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
            ultrasonicEvents = {
                flow {
                    ultrasonicStarts += 1
                    kotlinx.coroutines.awaitCancellation()
                }
            },
        )
        runCurrent()

        settings.value = settings.value.copy(wifiEnabled = !settings.value.wifiEnabled)
        runCurrent()
        settings.value = settings.value.copy(stalkerEnabled = false)
        runCurrent()

        assertEquals(1, bleStarts)
        assertEquals(1, ultrasonicStarts)
    }

    @Test
    fun ultrasonicReadyPublishesLiveEmptySourceSuccess() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(ultrasonicEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(audioCapture = true))
        val events = MutableSharedFlow<UltrasonicScanEvent>(extraBufferCapacity = 1)
        val clock = FakeClock(elapsed = 7_000L, wall = 70_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            ultrasonicEvents = { events },
        )
        runCurrent()

        assertEquals(SourceHealthState.LOADING, adapter.ultrasonicSnapshot().health.state)
        events.emit(UltrasonicScanEvent.Ready)
        runCurrent()

        assertEquals(SourceHealthState.LIVE, adapter.ultrasonicSnapshot().health.state)
        assertEquals(7_000L, adapter.ultrasonicSnapshot().health.lastSuccessElapsedMs)
        assertEquals(70_000L, adapter.ultrasonicSnapshot().health.lastSuccessWallMs)
        assertTrue(adapter.ultrasonicSnapshot().findings.isEmpty())
    }

    @Test
    fun repeatedReadyHeartbeatKeepsAnEmptyBleSourceHealthy() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 2)
        val clock = FakeClock(elapsed = 1_000L, wall = 10_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            bleEvents = { events },
        )
        runCurrent()

        events.emit(GlassesScanEvent.Ready)
        runCurrent()
        clock.elapsed = 20_000L
        clock.wall = 29_000L
        events.emit(GlassesScanEvent.Ready)
        runCurrent()

        val reduced = PrivacyCurrentReducer().reduce(
            sources = adapter.snapshots.value,
            ignoredKeys = emptySet(),
            nowElapsedMs = 49_999L,
        )
        assertEquals(SourceHealthState.LIVE, reduced.sources.single {
            it.source == PrivacySourceKind.PHONE_BLE
        }.state)
        assertTrue(reduced.findings.isEmpty())
    }

    @Test
    fun readyAfterFailureMakesAThenCompletedBleFlowFailAsUnexpectedlyStopped() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = {
                flowOf(
                    GlassesScanEvent.Failure("temporary scanner error"),
                    GlassesScanEvent.Ready,
                )
            },
        )
        runCurrent()

        assertEquals(SourceHealthState.FAILED, adapter.bleSnapshot().health.state)
        assertEquals(
            "BLE privacy scan stopped unexpectedly",
            adapter.bleSnapshot().health.message,
        )
    }

    @Test
    fun readyHeartbeatUpdatesHealthWithoutRestampingCachedBleFinding() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 2)
        val clock = FakeClock(elapsed = 5_000L, wall = 50_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            bleEvents = { events },
        )
        runCurrent()

        events.emit(GlassesScanEvent.Observation(glasses("fp:one", "AA:BB")))
        runCurrent()
        val observedAt = adapter.bleSnapshot().findings.single().lastObservedElapsedMs
        clock.elapsed = 20_000L
        clock.wall = 65_000L
        events.emit(GlassesScanEvent.Ready)
        runCurrent()

        assertEquals(20_000L, adapter.bleSnapshot().health.lastSuccessElapsedMs)
        assertEquals(observedAt, adapter.bleSnapshot().findings.single().lastObservedElapsedMs)
    }

    @Test
    fun ultrasonicObservationGetsOneIngestionIdentityAndTimestampThatTicksDoNotRewrite() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(ultrasonicEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(audioCapture = true))
        val events = MutableSharedFlow<UltrasonicScanEvent>(extraBufferCapacity = 4)
        val clock = FakeClock(elapsed = 5_000L, wall = 50_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            ultrasonicEvents = { events },
        )
        runCurrent()

        events.emit(
            UltrasonicScanEvent.Observation(
                UltrasonicDetector.UltrasonicAlert(
                    frequencyHz = 19_200f,
                    magnitudeDb = 40f,
                    noiseFloorDb = 20f,
                    snrDb = 20f,
                    persistenceFrames = 4,
                ),
            ),
        )
        runCurrent()
        val initial = adapter.ultrasonicSnapshot().findings.single()
        assertEquals(PrivacySourceKind.PHONE_ULTRASONIC, initial.source)
        assertEquals(5_000L, initial.lastObservedElapsedMs)
        assertEquals(50_000L, initial.lastSeenWallMs)
        assertTrue(initial.stableSourceId == null)

        clock.elapsed = Long.MAX_VALUE
        clock.wall = 1L
        runCurrent()
        val unchanged = adapter.ultrasonicSnapshot().findings.single()
        assertEquals(initial.observationKey, unchanged.observationKey)
        assertEquals(initial.lastObservedElapsedMs, unchanged.lastObservedElapsedMs)
        assertEquals(initial.lastSeenWallMs, unchanged.lastSeenWallMs)
    }

    @Test
    fun ultrasonicFailureAndUnsupportedRetainCachedRowsWithExactHealth() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(ultrasonicEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(audioCapture = true))
        val events = MutableSharedFlow<UltrasonicScanEvent>(extraBufferCapacity = 4)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            ultrasonicEvents = { events },
        )
        runCurrent()

        events.emit(
            UltrasonicScanEvent.Observation(
                UltrasonicDetector.UltrasonicAlert(19_000f, 40f, 20f, 20f, 3),
            ),
        )
        runCurrent()
        val key = adapter.ultrasonicSnapshot().findings.single().observationKey

        events.emit(UltrasonicScanEvent.Failure("audio read failed"))
        runCurrent()
        assertEquals(SourceHealthState.FAILED, adapter.ultrasonicSnapshot().health.state)
        assertEquals(key, adapter.ultrasonicSnapshot().findings.single().observationKey)

        events.emit(UltrasonicScanEvent.Unsupported("no ultrasonic input"))
        runCurrent()
        assertEquals(SourceHealthState.UNSUPPORTED, adapter.ultrasonicSnapshot().health.state)
        assertEquals(key, adapter.ultrasonicSnapshot().findings.single().observationKey)
    }

    @Test
    fun runtimeSecurityEventsBecomePermissionBlockedForOnlyTheirExactPhoneSource() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                ultrasonicEnabled = true,
            ),
        )
        val permissions = MutableStateFlow(
            LocalDetectionPermissions.None.copy(bluetoothScan = true, audioCapture = true),
        )
        val bleEvents = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 1)
        val ultrasonicEvents = MutableSharedFlow<UltrasonicScanEvent>(extraBufferCapacity = 1)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { bleEvents },
            ultrasonicEvents = { ultrasonicEvents },
        )
        runCurrent()

        bleEvents.emit(GlassesScanEvent.PermissionBlocked("Nearby devices permission was revoked"))
        runCurrent()
        assertEquals(SourceHealthState.PERMISSION_BLOCKED, adapter.bleSnapshot().health.state)
        assertEquals(SourceHealthState.LOADING, adapter.ultrasonicSnapshot().health.state)

        ultrasonicEvents.emit(
            UltrasonicScanEvent.PermissionBlocked("Microphone permission was revoked"),
        )
        runCurrent()
        assertEquals(SourceHealthState.PERMISSION_BLOCKED, adapter.ultrasonicSnapshot().health.state)
    }

    @Test
    fun repeatedUltrasonicFramesInOneFrequencyBinReuseTheObservationIdentity() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(ultrasonicEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(audioCapture = true))
        val events = MutableSharedFlow<UltrasonicScanEvent>(extraBufferCapacity = 4)
        val clock = FakeClock(elapsed = 5_000L, wall = 50_000L)
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            clock = clock,
            ultrasonicEvents = { events },
        )
        runCurrent()

        events.emit(
            UltrasonicScanEvent.Observation(
                UltrasonicDetector.UltrasonicAlert(19_201f, 40f, 20f, 20f, 3),
            ),
        )
        runCurrent()
        val firstKey = adapter.ultrasonicSnapshot().findings.single().observationKey
        clock.elapsed = 6_000L
        clock.wall = 51_000L
        events.emit(
            UltrasonicScanEvent.Observation(
                UltrasonicDetector.UltrasonicAlert(19_225f, 42f, 20f, 22f, 4),
            ),
        )
        runCurrent()

        assertEquals(1, adapter.ultrasonicSnapshot().findings.size)
        assertEquals(firstKey, adapter.ultrasonicSnapshot().findings.single().observationKey)
        assertEquals(6_000L, adapter.ultrasonicSnapshot().findings.single().lastObservedElapsedMs)
    }

    @Test
    fun repeatedNoFingerprintSamplesReuseOneBoundedEphemeralObservation() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 4)
        val clock = FakeClock(elapsed = 10_000L, wall = 100_000L)
        val adapter = adapter(settings, permissions, bleEvents = { events }, clock = clock)
        runCurrent()
        val raw = glasses("", "AA:BB")

        events.emit(GlassesScanEvent.Observation(raw))
        runCurrent()
        val firstKey = adapter.bleSnapshot().findings.single().observationKey
        clock.elapsed = 11_000L
        events.emit(GlassesScanEvent.Observation(raw.copy(rssi = -40)))
        runCurrent()

        assertEquals(1, adapter.bleSnapshot().findings.size)
        assertEquals(firstKey, adapter.bleSnapshot().findings.single().observationKey)
        assertEquals(11_000L, adapter.bleSnapshot().findings.single().lastObservedElapsedMs)
    }

    @Test
    fun pineappleTextDoesNotCreateAppleListeningEvidenceAndBlankEvidenceStaysUnknown() {
        val raw = glasses("fp:pineapple", "AA:BB").copy(
            manufacturer = "Pineapple Labs",
            deviceName = "Pineapple microphone tester",
            deviceType = "Possible Listening",
            matchReason = "",
            category = PrivacyCategory.REMOTE_LISTENING,
            bleCompanyId = null,
        )

        val finding = PhonePrivacySourceAdapter.mapBle(raw, 1_000L, 10_000L)

        assertEquals(PrivacyCategory.REMOTE_LISTENING, finding.category)
        assertEquals(FindingSeverity.AWARENESS, finding.severity)
        assertEquals("Pineapple Labs", finding.evidence)

        val unknown = PhonePrivacySourceAdapter.mapBle(
            raw.copy(manufacturer = "", deviceName = null),
            1_000L,
            10_000L,
        )
        assertEquals(null, unknown.evidence)
    }

    @Test
    fun followerAnalysisRunsOnCadenceInsteadOfEveryBlePacket() = runTest {
        val now = Instant.now()
        val tracker = BleTracker()
        recordQualifyingFollowerHistory(tracker, now)
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 4)
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { events },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = tracker,
            clock = FakeClock(),
            scope = backgroundScope,
            followerPollIntervalMs = 100L,
        )
        runCurrent()

        assertTrue(adapter.bleSnapshot().findings.isEmpty())
        advanceTimeBy(99L)
        runCurrent()
        assertTrue(adapter.bleSnapshot().findings.isEmpty())
        advanceTimeBy(1L)
        runCurrent()

        assertTrue(adapter.bleSnapshot().findings.any {
            it.observationKey.sourceRecordId == "follower:CC:DD"
        })
    }

    @Test
    fun recurringFollowerPollsPreserveTheLastRealBleObservationTime() = runTest {
        val now = Instant.now()
        val tracker = BleTracker()
        recordQualifyingFollowerHistory(tracker, now)
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val clock = FakeClock(elapsed = 100_000L, wall = now.toEpochMilli())
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = tracker,
            clock = clock,
            scope = backgroundScope,
            followerPollIntervalMs = 100L,
        )
        runCurrent()

        advanceTimeBy(100L)
        runCurrent()
        val first = adapter.bleSnapshot().findings.single {
            it.observationKey.sourceRecordId == "follower:CC:DD"
        }

        clock.elapsed = 200_000L
        clock.wall += 100_000L
        advanceTimeBy(100L)
        runCurrent()
        val repeated = adapter.bleSnapshot().findings.single {
            it.observationKey.sourceRecordId == "follower:CC:DD"
        }

        assertEquals(first.lastObservedElapsedMs, repeated.lastObservedElapsedMs)
        assertEquals(first.lastSeenWallMs, repeated.lastSeenWallMs)
    }

    @Test
    fun disablingStalkerAnalysisImmediatelyRemovesFollowerRowsButKeepsOrdinaryBleFindings() = runTest {
        val now = Instant.now()
        val tracker = BleTracker()
        recordQualifyingFollowerHistory(tracker, now)
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = true),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 1)
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { events },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = tracker,
            clock = FakeClock(),
            scope = backgroundScope,
            followerPollIntervalMs = 100L,
        )
        runCurrent()
        events.emit(GlassesScanEvent.Observation(glasses("fp:ordinary", "AA:BB")))
        runCurrent()
        advanceTimeBy(100L)
        runCurrent()
        assertTrue(adapter.bleSnapshot().findings.any {
            it.observationKey.sourceRecordId == "follower:CC:DD"
        })

        settings.value = settings.value.copy(stalkerEnabled = false)
        runCurrent()

        assertFalse(adapter.bleSnapshot().findings.any {
            it.observationKey.sourceRecordId.startsWith("follower:")
        })
        assertTrue(adapter.bleSnapshot().findings.any {
            it.stableSourceId == "fp:ordinary"
        })
        assertEquals(SourceHealthState.LIVE, adapter.bleSnapshot().health.state)
    }

    @Test
    fun disabledStalkerAnalysisKeepsDirectionSamplesWithoutMovementHistoryOrFollowerAlerts() = runTest {
        val now = Instant.now()
        val tracker = BleTracker()
        tracker.recordSightingAt(
            "CC:DD", -60, "Tag", "BLE Tracker", "Generic", false,
            37.0, -122.0, 0f, now.minusSeconds(181),
        )
        tracker.recordSightingAt(
            "CC:DD", -59, "Tag", "BLE Tracker", "Generic", false,
            37.0, -122.0, 0f, now.minusSeconds(120),
        )
        tracker.recordSightingAt(
            "CC:DD", -58, "Tag", "BLE Tracker", "Generic", false,
            37.0, -122.0, 0f, now.minusSeconds(60),
        )
        tracker.startDirectionScan("AA:BB")
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                stalkerEnabled = false,
            ),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None.copy(bluetoothScan = true))
        val events = MutableSharedFlow<GlassesScanEvent>(extraBufferCapacity = 1)
        val adapter = PhonePrivacySourceAdapter(
            settings = settings,
            permissions = permissions,
            bleEvents = { events },
            ultrasonicEvents = { flow { kotlinx.coroutines.awaitCancellation() } },
            bleTracker = tracker,
            clock = FakeClock(),
            scope = backgroundScope,
            followerPollIntervalMs = 100L,
        )
        runCurrent()

        events.emit(GlassesScanEvent.Observation(glasses("fp:meta-one", "AA:BB")))
        runCurrent()
        assertEquals(null, tracker.getDevice("AA:BB"))
        assertEquals(1, tracker.getDirectionSampleCount())

        advanceTimeBy(100L)
        runCurrent()
        assertFalse(adapter.bleSnapshot().findings.any {
            it.observationKey.sourceRecordId == "follower:CC:DD"
        })
    }

    private fun kotlinx.coroutines.test.TestScope.adapter(
        settings: MutableStateFlow<DetectionSettings>,
        permissions: MutableStateFlow<LocalDetectionPermissions>,
        bleEvents: () -> Flow<GlassesScanEvent> = { flow { kotlinx.coroutines.awaitCancellation() } },
        ultrasonicEvents: () -> Flow<UltrasonicScanEvent> = { flow { kotlinx.coroutines.awaitCancellation() } },
        bleTracker: BleTracker = BleTracker(),
        clock: FakeClock = FakeClock(),
    ) = PhonePrivacySourceAdapter(
        settings = settings,
        permissions = permissions,
        bleEvents = bleEvents,
        ultrasonicEvents = ultrasonicEvents,
        bleTracker = bleTracker,
        clock = clock,
        scope = backgroundScope,
    )

    private fun recordQualifyingFollowerHistory(tracker: BleTracker, now: Instant) {
        val latitudes = listOf(37.0000, 37.0000, 37.0008, 37.0008, 37.0017, 37.0017)
        latitudes.forEachIndexed { index, latitude ->
            tracker.recordSightingAt(
                mac = "CC:DD",
                rssi = -60 + index,
                deviceName = "Tag",
                deviceType = "BLE Tracker",
                manufacturer = "Generic",
                hasCamera = false,
                userLat = latitude,
                userLon = -122.0,
                compassBearing = 0f,
                timestamp = now.minusSeconds(301L - index * 60L),
                category = PrivacyCategory.BLE_TRACKER,
                locationAccuracyMeters = 5f,
            )
        }
    }

    private fun PhonePrivacySourceAdapter.bleSnapshot(): PrivacySourceSnapshot =
        snapshots.value.single { it.health.source == PrivacySourceKind.PHONE_BLE }

    private fun PhonePrivacySourceAdapter.ultrasonicSnapshot(): PrivacySourceSnapshot =
        snapshots.value.single { it.health.source == PrivacySourceKind.PHONE_ULTRASONIC }

    private fun glasses(fingerprint: String, mac: String) = GlassesDetection(
        mac = mac,
        deviceName = "Meta glasses",
        deviceType = "Smart Glasses",
        manufacturer = "Meta",
        hasCamera = true,
        rssi = -52,
        confidence = 0.92f,
        matchReason = "mfr",
        firstSeen = Instant.ofEpochMilli(90_000L),
        lastSeen = Instant.ofEpochMilli(100_000L),
        category = PrivacyCategory.SMART_GLASSES,
        fingerprintKey = fingerprint,
    )

    private class FakeClock(
        var elapsed: Long = 1_000L,
        var wall: Long = 10_000L,
    ) : MonotonicClock {
        override fun nowElapsedMs(): Long = elapsed
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(wall)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }
}
