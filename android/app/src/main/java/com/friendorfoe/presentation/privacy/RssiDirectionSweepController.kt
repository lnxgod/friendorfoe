package com.friendorfoe.presentation.privacy

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withTimeoutOrNull

data class RssiSample(
    val findingKey: PrivacyFindingKey,
    val dbm: Int,
    val azimuthDegrees: Float,
    val observedAtElapsedMs: Long,
)

sealed interface DirectionSweepState {
    data object Idle : DirectionSweepState
    data class Sampling(
        val key: PrivacyFindingKey,
        val sampleCount: Int = 0,
        val currentDbm: Int? = null,
    ) : DirectionSweepState
    data class InsufficientSamples(val count: Int) : DirectionSweepState
    data class Complete(val key: PrivacyFindingKey) : DirectionSweepState
}

fun interface RssiSampleSource {
    fun samplesFor(key: PrivacyFindingKey): Flow<RssiSample>
}

fun summarizeStrongestDirection(samples: List<RssiSample>): String {
    require(samples.isNotEmpty())
    val strongestSector = samples.groupBy { sample ->
        ((((sample.azimuthDegrees % 360f) + 360f) % 360f) / 45f)
            .toInt()
            .coerceIn(0, 7)
    }.maxWithOrNull(
        compareBy<Map.Entry<Int, List<RssiSample>>> {
            it.value.map(RssiSample::dbm).average()
        }.thenBy { -it.key },
    )?.key ?: error("A non-empty sample list requires a strongest sector")
    val start = strongestSector * 45
    return "Strongest signal was toward $start°–${start + 44}° during this sweep."
}

class RssiDirectionSweepController(
    private val sampleSource: RssiSampleSource,
    private val scope: CoroutineScope,
) {
    private var samplingJob: Job? = null
    private var targetKey: PrivacyFindingKey? = null
    private val captured = mutableListOf<RssiSample>()
    private val _state = MutableStateFlow<DirectionSweepState>(DirectionSweepState.Idle)
    val state = _state.asStateFlow()
    private val _resultText = MutableStateFlow("")
    val resultText = _resultText.asStateFlow()

    fun start(finding: PrivacyFinding) {
        if (!finding.capabilities.canOpenDirectionSweep ||
            finding.source != PrivacySourceKind.PHONE_BLE ||
            finding.freshness != FindingFreshness.LIVE
        ) {
            return
        }
        val key = finding.observationKey
        samplingJob?.cancel()
        targetKey = key
        synchronized(captured) { captured.clear() }
        _resultText.value = ""
        _state.value = DirectionSweepState.Sampling(key)
        samplingJob = scope.launch {
            withTimeoutOrNull(SWEEP_TIMEOUT_MS) {
                sampleSource.samplesFor(key)
                    .filter { sample ->
                        sample.findingKey == key &&
                            sample.azimuthDegrees.isFinite() &&
                            sample.dbm in -127..20
                    }
                    .take(MAX_SAMPLES)
                    .collect { sample ->
                        val count = synchronized(captured) {
                            captured += sample
                            captured.size
                        }
                        _state.value = DirectionSweepState.Sampling(
                            key = key,
                            sampleCount = count,
                            currentDbm = sample.dbm,
                        )
                    }
            }
            if (currentCoroutineContext().isActive && targetKey == key) {
                completeFromCaptured()
            }
        }
    }

    fun finish() {
        if (targetKey == null) return
        samplingJob?.cancel()
        samplingJob = null
        completeFromCaptured()
    }

    fun cancel() {
        samplingJob?.cancel()
        samplingJob = null
        targetKey = null
        synchronized(captured) { captured.clear() }
        _resultText.value = ""
        _state.value = DirectionSweepState.Idle
    }

    private fun completeFromCaptured() {
        val key = targetKey ?: return
        samplingJob = null
        val snapshot = synchronized(captured) { captured.toList() }
        if (snapshot.size < MIN_SAMPLES) {
            _resultText.value = "Not enough samples; keep turning slowly and try again."
            _state.value = DirectionSweepState.InsufficientSamples(snapshot.size)
            return
        }
        _resultText.value = summarizeStrongestDirection(snapshot) +
            " Signal strength is approximate and does not locate the device."
        _state.value = DirectionSweepState.Complete(key)
    }

    private companion object {
        const val MIN_SAMPLES = 6
        const val MAX_SAMPLES = 40
        const val SWEEP_TIMEOUT_MS = 30_000L
    }
}
