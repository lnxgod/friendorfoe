package com.friendorfoe.detection

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import com.friendorfoe.data.time.MonotonicClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.transform
import kotlinx.coroutines.isActive
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.coroutineContext
import kotlin.math.cos
import kotlin.math.ln
import kotlin.math.sqrt

internal interface UltrasonicAudioCapture {
    val initialized: Boolean
    val recording: Boolean
    fun start()
    fun read(buffer: ShortArray, offset: Int, length: Int): Int
    fun stop()
    fun release()
}

internal interface UltrasonicAudioPlatform {
    fun minimumBufferSize(sampleRate: Int, channelConfig: Int, audioFormat: Int): Int

    fun createCapture(
        audioSource: Int,
        sampleRate: Int,
        channelConfig: Int,
        audioFormat: Int,
        bufferSize: Int,
    ): UltrasonicAudioCapture
}

private object AndroidUltrasonicAudioPlatform : UltrasonicAudioPlatform {
    override fun minimumBufferSize(
        sampleRate: Int,
        channelConfig: Int,
        audioFormat: Int,
    ): Int = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)

    @Suppress("MissingPermission")
    override fun createCapture(
        audioSource: Int,
        sampleRate: Int,
        channelConfig: Int,
        audioFormat: Int,
        bufferSize: Int,
    ): UltrasonicAudioCapture = AndroidUltrasonicAudioCapture(
        AudioRecord(audioSource, sampleRate, channelConfig, audioFormat, bufferSize),
    )
}

private class AndroidUltrasonicAudioCapture(
    private val audioRecord: AudioRecord,
) : UltrasonicAudioCapture {
    override val initialized: Boolean
        get() = audioRecord.state == AudioRecord.STATE_INITIALIZED
    override val recording: Boolean
        get() = audioRecord.recordingState == AudioRecord.RECORDSTATE_RECORDING

    override fun start() = audioRecord.startRecording()
    override fun read(buffer: ShortArray, offset: Int, length: Int): Int =
        audioRecord.read(buffer, offset, length)
    override fun stop() = audioRecord.stop()
    override fun release() = audioRecord.release()
}

sealed interface UltrasonicScanEvent {
    data object Ready : UltrasonicScanEvent
    data class Observation(val alert: UltrasonicDetector.UltrasonicAlert) : UltrasonicScanEvent
    data class Failure(val message: String) : UltrasonicScanEvent
    data class PermissionBlocked(val message: String) : UltrasonicScanEvent
    data class Unsupported(val message: String) : UltrasonicScanEvent
}

/**
 * Detects ultrasonic tracking beacons (18-22 kHz) that are inaudible
 * to humans but used by advertising/tracking SDKs (SilverPush, Lisnr,
 * Shopkick, Signal360) to track users across devices.
 *
 * Uses AudioRecord at 48 kHz sample rate with FFT analysis.
 * Requires RECORD_AUDIO permission.
 */
@Singleton
class UltrasonicDetector internal constructor(
    private val audioPlatform: UltrasonicAudioPlatform,
    private val clock: MonotonicClock,
    private val dispatcher: CoroutineDispatcher,
) {

    @Inject
    constructor(clock: MonotonicClock) : this(
        audioPlatform = AndroidUltrasonicAudioPlatform,
        clock = clock,
        dispatcher = Dispatchers.Default,
    )

    companion object {
        private const val TAG = "UltrasonicDetector"
        private const val SAMPLE_RATE = 48000
        private const val FFT_SIZE = 4096 // ~85ms window at 48kHz
        private const val MIN_FREQ_HZ = 17500f
        private const val MAX_FREQ_HZ = 22000f
        private const val DETECTION_THRESHOLD_DB = 12f // above noise floor
        private const val MIN_PERSISTENCE_FRAMES = 3 // ~255ms at 48kHz/4096
        private const val HEALTH_HEARTBEAT_MS = 15_000L
    }

    data class UltrasonicAlert(
        val frequencyHz: Float,
        val magnitudeDb: Float,
        val noiseFloorDb: Float,
        val snrDb: Float,
        val persistenceFrames: Int
    )

    fun monitoringEvents(): Flow<UltrasonicScanEvent> = flow {
        var recorder: UltrasonicAudioCapture? = null
        try {
            val bufferSize = audioPlatform.minimumBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            ).coerceAtLeast(FFT_SIZE * 2)

            val audioSource = try {
                val source = MediaRecorder.AudioSource.UNPROCESSED
                val test = audioPlatform.createCapture(
                    audioSource = source,
                    sampleRate = SAMPLE_RATE,
                    channelConfig = AudioFormat.CHANNEL_IN_MONO,
                    audioFormat = AudioFormat.ENCODING_PCM_16BIT,
                    bufferSize = bufferSize,
                )
                val selected = if (test.initialized) {
                    source
                } else {
                    MediaRecorder.AudioSource.MIC
                }
                test.release()
                selected
            } catch (_: Exception) {
                MediaRecorder.AudioSource.MIC
            }

            recorder = audioPlatform.createCapture(
                audioSource = audioSource,
                sampleRate = SAMPLE_RATE,
                channelConfig = AudioFormat.CHANNEL_IN_MONO,
                audioFormat = AudioFormat.ENCODING_PCM_16BIT,
                bufferSize = bufferSize,
            )
            val activeRecorder = recorder
            if (!activeRecorder.initialized) {
                safeLogError("AudioRecord failed to initialize")
                emit(UltrasonicScanEvent.Unsupported("Ultrasonic audio capture is unavailable"))
                return@flow
            }

            activeRecorder.start()
            if (!activeRecorder.recording) {
                emit(UltrasonicScanEvent.Failure("Microphone capture did not start"))
                return@flow
            }
            safeLogInfo(
                "Ultrasonic monitoring started (${SAMPLE_RATE}Hz, source=$audioSource, FFT=$FFT_SIZE)",
            )
            var lastHeartbeatElapsedMs = clock.nowElapsedMs()
            emit(UltrasonicScanEvent.Ready)

            val buffer = ShortArray(FFT_SIZE)
            val window = hannWindow(FFT_SIZE)
            val persistenceMap = mutableMapOf<Int, Int>()
            while (coroutineContext.isActive) {
                val read = activeRecorder.read(buffer, 0, FFT_SIZE)
                if (read < 0) {
                    emit(UltrasonicScanEvent.Failure("Microphone read failed ($read)"))
                    return@flow
                }
                if (read > 0) {
                    val nowElapsedMs = clock.nowElapsedMs()
                    if (elapsedSince(nowElapsedMs, lastHeartbeatElapsedMs) >= HEALTH_HEARTBEAT_MS) {
                        lastHeartbeatElapsedMs = nowElapsedMs
                        emit(UltrasonicScanEvent.Ready)
                    }
                }
                if (read < FFT_SIZE) continue

                // Apply Hann window and convert to doubles
                val windowed = DoubleArray(FFT_SIZE) { buffer[it].toDouble() * window[it] }

                // Compute magnitude spectrum via DFT (real-valued input)
                val magnitudes = computeMagnitudeSpectrum(windowed)

                // Find frequency range bins
                val binResolution = SAMPLE_RATE.toFloat() / FFT_SIZE
                val minBin = (MIN_FREQ_HZ / binResolution).toInt()
                val maxBin = (MAX_FREQ_HZ / binResolution).toInt().coerceAtMost(magnitudes.size - 1)

                // Calculate noise floor (median of all bins below 17 kHz)
                val noiseBins = (10 until minBin).map { magnitudes[it] }.sorted()
                val noiseFloor = if (noiseBins.isNotEmpty()) {
                    noiseBins[noiseBins.size / 2]
                } else 1.0

                val noiseFloorDb = 20.0 * ln(noiseFloor.coerceAtLeast(1.0)) / ln(10.0)

                // Check for peaks in ultrasonic range
                var foundPeak = false
                for (bin in minBin..maxBin) {
                    val magDb = 20.0 * ln(magnitudes[bin].coerceAtLeast(1.0)) / ln(10.0)
                    val snr = magDb - noiseFloorDb

                    if (snr > DETECTION_THRESHOLD_DB) {
                        val count = (persistenceMap[bin] ?: 0) + 1
                        persistenceMap[bin] = count
                        foundPeak = true

                        if (count >= MIN_PERSISTENCE_FRAMES) {
                            val freq = bin * binResolution
                            emit(
                                UltrasonicScanEvent.Observation(
                                    UltrasonicAlert(
                                        frequencyHz = freq,
                                        magnitudeDb = magDb.toFloat(),
                                        noiseFloorDb = noiseFloorDb.toFloat(),
                                        snrDb = snr.toFloat(),
                                        persistenceFrames = count,
                                    ),
                                ),
                            )
                            safeLogWarning(
                                "ULTRASONIC BEACON: %.0fHz SNR=%.1fdB (%d frames)"
                                    .format(freq, snr, count),
                            )
                        }
                    } else {
                        persistenceMap.remove(bin)
                    }
                }

                if (!foundPeak) {
                    // Clear all persistence counters in the ultrasonic range
                    for (bin in minBin..maxBin) persistenceMap.remove(bin)
                }
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (unsupported: IllegalArgumentException) {
            emit(
                UltrasonicScanEvent.Unsupported(
                    unsupported.message ?: "Ultrasonic audio capture is unsupported",
                ),
            )
        } catch (security: SecurityException) {
            emit(
                UltrasonicScanEvent.PermissionBlocked(
                    security.message ?: "Microphone permission was revoked",
                ),
            )
        } catch (failure: Throwable) {
            emit(UltrasonicScanEvent.Failure(failure.message ?: "Ultrasonic capture failed"))
        } finally {
            recorder?.let { activeRecorder ->
                if (activeRecorder.recording) {
                    runCatching { activeRecorder.stop() }
                }
                runCatching { activeRecorder.release() }
                safeLogInfo("Ultrasonic monitoring stopped")
            }
        }
    }.flowOn(dispatcher)

    /** Legacy observation-only view; lifecycle ownership belongs to the event collector. */
    fun startMonitoring(): Flow<UltrasonicAlert> = monitoringEvents().transform { event ->
        if (event is UltrasonicScanEvent.Observation) {
            emit(event.alert)
        }
    }

    private fun hannWindow(size: Int): DoubleArray {
        return DoubleArray(size) { 0.5 * (1.0 - cos(2.0 * Math.PI * it / (size - 1))) }
    }

    private fun elapsedSince(nowElapsedMs: Long, earlierElapsedMs: Long): Long {
        if (nowElapsedMs <= earlierElapsedMs) return 0L
        val difference = nowElapsedMs - earlierElapsedMs
        return if (difference < 0L) Long.MAX_VALUE else difference
    }

    private fun safeLogInfo(message: String) {
        try {
            Log.i(TAG, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }

    private fun safeLogWarning(message: String) {
        try {
            Log.w(TAG, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }

    private fun safeLogError(message: String) {
        try {
            Log.e(TAG, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }

    /**
     * Simple magnitude spectrum via DFT for the ultrasonic range only.
     * We only need bins from ~17kHz to ~22kHz so we compute those directly
     * rather than a full FFT, which is actually slower for partial ranges
     * but simpler. For a 4096-point FFT at 48kHz, the full spectrum is fine.
     */
    private fun computeMagnitudeSpectrum(x: DoubleArray): DoubleArray {
        val n = x.size
        val halfN = n / 2
        val mags = DoubleArray(halfN)

        // Radix-2 FFT (in-place, Cooley-Tukey)
        val real = x.copyOf()
        val imag = DoubleArray(n)

        // Bit-reversal permutation
        var j = 0
        for (i in 0 until n) {
            if (i < j) {
                val tr = real[j]; real[j] = real[i]; real[i] = tr
                val ti = imag[j]; imag[j] = imag[i]; imag[i] = ti
            }
            var m = n / 2
            while (m >= 1 && j >= m) { j -= m; m /= 2 }
            j += m
        }

        // FFT butterfly
        var step = 1
        while (step < n) {
            val halfStep = step
            step *= 2
            val wReal = cos(Math.PI / halfStep)
            val wImag = -kotlin.math.sin(Math.PI / halfStep)
            var wr = 1.0
            var wi = 0.0
            for (m2 in 0 until halfStep) {
                for (i in m2 until n step step) {
                    val k = i + halfStep
                    val tr = wr * real[k] - wi * imag[k]
                    val ti = wr * imag[k] + wi * real[k]
                    real[k] = real[i] - tr
                    imag[k] = imag[i] - ti
                    real[i] += tr
                    imag[i] += ti
                }
                val newWr = wr * wReal - wi * wImag
                wi = wr * wImag + wi * wReal
                wr = newWr
            }
        }

        for (i in 0 until halfN) {
            mags[i] = sqrt(real[i] * real[i] + imag[i] * imag[i])
        }
        return mags
    }
}
