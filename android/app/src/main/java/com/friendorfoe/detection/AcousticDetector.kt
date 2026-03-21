package com.friendorfoe.detection

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.coroutineContext
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.log10
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * FFT-based acoustic engine signature detector.
 *
 * Uses the phone microphone to classify propulsion sounds into frequency bands
 * that correspond to known drone/aircraft engine types. No ML training required —
 * frequency bands are physics-based constants from engine specifications.
 *
 * Frequency bands:
 * - Piston engine: 80-400 Hz (Shahed-136 Mado MD-550 signature)
 * - Large turbine: 200-2000 Hz (commercial jet/turboprop)
 * - Small electric motor: 2-8 kHz (consumer drone brushless / Lancet)
 * - Ambient baseline: full-spectrum noise floor
 *
 * Limitation: Phone microphones have limited range. Useful as confirmation
 * signal when combined with visual tracking, not as primary detection.
 */
@Singleton
class AcousticDetector @Inject constructor(
    @ApplicationContext private val context: Context
) {

    companion object {
        private const val TAG = "AcousticDetector"
        private const val SAMPLE_RATE = 44100
        private const val FFT_SIZE = 1024
        private const val CHUNK_MS = 250L

        // Frequency band edges (Hz)
        private const val PISTON_LOW = 80f
        private const val PISTON_HIGH = 400f
        private const val TURBINE_LOW = 200f
        private const val TURBINE_HIGH = 2000f
        private const val ELECTRIC_LOW = 2000f
        private const val ELECTRIC_HIGH = 8000f

        /** Minimum energy ratio above noise floor to report a detection */
        private const val MIN_DETECTION_RATIO = 3.0f
    }

    private val _lastResult = MutableStateFlow(AcousticResult.NONE)
    val lastResult = _lastResult.asStateFlow()

    /**
     * Start continuous acoustic monitoring. Emits [AcousticResult] for each
     * analysis chunk (~4 Hz). Stops when the collecting coroutine is cancelled.
     */
    fun startMonitoring(): Flow<AcousticResult> = flow {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            Log.w(TAG, "RECORD_AUDIO permission not granted, acoustic detection disabled")
            emit(AcousticResult.NONE)
            return@flow
        }

        val bufferSize = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(FFT_SIZE * 2)

        val recorder = try {
            @Suppress("MissingPermission")
            AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferSize
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create AudioRecord", e)
            emit(AcousticResult.NONE)
            return@flow
        }

        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord failed to initialize")
            recorder.release()
            emit(AcousticResult.NONE)
            return@flow
        }

        try {
            recorder.startRecording()
            Log.i(TAG, "Acoustic monitoring started")

            val buffer = ShortArray(FFT_SIZE)

            while (coroutineContext.isActive) {
                val read = recorder.read(buffer, 0, FFT_SIZE)
                if (read < FFT_SIZE) continue

                val result = analyzeChunk(buffer)
                _lastResult.value = result
                emit(result)
            }
        } finally {
            try {
                recorder.stop()
            } catch (_: Exception) {}
            recorder.release()
            Log.i(TAG, "Acoustic monitoring stopped")
        }
    }.flowOn(Dispatchers.IO)

    /**
     * Analyze a single audio chunk via FFT and classify by frequency band energy.
     */
    private fun analyzeChunk(samples: ShortArray): AcousticResult {
        // Convert to float and apply Hanning window
        val real = FloatArray(FFT_SIZE)
        val imag = FloatArray(FFT_SIZE)
        for (i in samples.indices) {
            val window = 0.5f * (1f - cos(2f * PI.toFloat() * i / (FFT_SIZE - 1)))
            real[i] = samples[i].toFloat() * window
        }

        // In-place FFT (Cooley-Tukey radix-2)
        fft(real, imag)

        // Compute power spectrum
        val freqResolution = SAMPLE_RATE.toFloat() / FFT_SIZE
        val halfSize = FFT_SIZE / 2

        var pistonEnergy = 0f
        var turbineEnergy = 0f
        var electricEnergy = 0f
        var totalEnergy = 0f
        var binCount = 0

        for (i in 1 until halfSize) {
            val freq = i * freqResolution
            val power = real[i] * real[i] + imag[i] * imag[i]
            totalEnergy += power
            binCount++

            when {
                freq in PISTON_LOW..PISTON_HIGH -> pistonEnergy += power
                freq in TURBINE_LOW..TURBINE_HIGH -> turbineEnergy += power
                freq in ELECTRIC_LOW..ELECTRIC_HIGH -> electricEnergy += power
            }
        }

        val noiseFloor = if (binCount > 0) totalEnergy / binCount else 1f
        if (noiseFloor < 1f) return AcousticResult.NONE

        // Normalize band energies by number of bins in each band
        val pistonBins = ((PISTON_HIGH - PISTON_LOW) / freqResolution).toInt().coerceAtLeast(1)
        val turbineBins = ((TURBINE_HIGH - TURBINE_LOW) / freqResolution).toInt().coerceAtLeast(1)
        val electricBins = ((ELECTRIC_HIGH - ELECTRIC_LOW) / freqResolution).toInt().coerceAtLeast(1)

        val pistonAvg = pistonEnergy / pistonBins
        val turbineAvg = turbineEnergy / turbineBins
        val electricAvg = electricEnergy / electricBins

        val pistonRatio = pistonAvg / noiseFloor
        val turbineRatio = turbineAvg / noiseFloor
        val electricRatio = electricAvg / noiseFloor

        // Classify based on dominant band
        return when {
            pistonRatio > MIN_DETECTION_RATIO && pistonRatio > turbineRatio &&
                    pistonRatio > electricRatio ->
                AcousticResult(
                    engineType = EngineType.PISTON,
                    confidence = (pistonRatio / 10f).coerceAtMost(1f),
                    dominantFrequencyHz = findPeakFrequency(real, imag, PISTON_LOW, PISTON_HIGH, freqResolution)
                )
            turbineRatio > MIN_DETECTION_RATIO && turbineRatio > electricRatio ->
                AcousticResult(
                    engineType = EngineType.TURBINE,
                    confidence = (turbineRatio / 10f).coerceAtMost(1f),
                    dominantFrequencyHz = findPeakFrequency(real, imag, TURBINE_LOW, TURBINE_HIGH, freqResolution)
                )
            electricRatio > MIN_DETECTION_RATIO ->
                AcousticResult(
                    engineType = EngineType.ELECTRIC_MOTOR,
                    confidence = (electricRatio / 10f).coerceAtMost(1f),
                    dominantFrequencyHz = findPeakFrequency(real, imag, ELECTRIC_LOW, ELECTRIC_HIGH, freqResolution)
                )
            else -> AcousticResult.NONE
        }
    }

    private fun findPeakFrequency(
        real: FloatArray, imag: FloatArray,
        lowHz: Float, highHz: Float, freqRes: Float
    ): Float {
        val lowBin = (lowHz / freqRes).toInt().coerceAtLeast(1)
        val highBin = (highHz / freqRes).toInt().coerceAtMost(real.size / 2 - 1)
        var peakPower = 0f
        var peakBin = lowBin
        for (i in lowBin..highBin) {
            val power = real[i] * real[i] + imag[i] * imag[i]
            if (power > peakPower) {
                peakPower = power
                peakBin = i
            }
        }
        return peakBin * freqRes
    }

    /** In-place Cooley-Tukey radix-2 FFT. Arrays must be power-of-2 length. */
    private fun fft(real: FloatArray, imag: FloatArray) {
        val n = real.size
        // Bit-reversal permutation
        var j = 0
        for (i in 0 until n - 1) {
            if (i < j) {
                var temp = real[i]; real[i] = real[j]; real[j] = temp
                temp = imag[i]; imag[i] = imag[j]; imag[j] = temp
            }
            var k = n / 2
            while (k <= j) { j -= k; k /= 2 }
            j += k
        }
        // Butterfly operations
        var len = 2
        while (len <= n) {
            val halfLen = len / 2
            val angle = -2.0 * PI / len
            for (i in 0 until n step len) {
                for (m in 0 until halfLen) {
                    val wr = cos(angle * m).toFloat()
                    val wi = sin(angle * m).toFloat()
                    val idx1 = i + m
                    val idx2 = i + m + halfLen
                    val tr = wr * real[idx2] - wi * imag[idx2]
                    val ti = wr * imag[idx2] + wi * real[idx2]
                    real[idx2] = real[idx1] - tr
                    imag[idx2] = imag[idx1] - ti
                    real[idx1] += tr
                    imag[idx1] += ti
                }
            }
            len *= 2
        }
    }
}

/** Detected engine type from acoustic analysis. */
enum class EngineType(val label: String) {
    /** 80-400 Hz — small piston engine (Shahed-136 Mado MD-550 signature) */
    PISTON("Piston engine"),
    /** 200-2000 Hz — jet or turboprop (commercial aircraft) */
    TURBINE("Turbine/jet"),
    /** 2-8 kHz — brushless electric motor (consumer drone or Lancet) */
    ELECTRIC_MOTOR("Electric motor"),
    /** No engine signature detected */
    NONE("None")
}

/** Result of acoustic frequency band analysis. */
data class AcousticResult(
    val engineType: EngineType,
    val confidence: Float = 0f,
    val dominantFrequencyHz: Float = 0f
) {
    companion object {
        val NONE = AcousticResult(EngineType.NONE, 0f, 0f)
    }
}
