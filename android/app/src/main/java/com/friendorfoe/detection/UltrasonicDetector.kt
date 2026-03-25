package com.friendorfoe.detection

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.coroutineContext
import kotlin.math.cos
import kotlin.math.ln
import kotlin.math.sqrt

/**
 * Detects ultrasonic tracking beacons (18-22 kHz) that are inaudible
 * to humans but used by advertising/tracking SDKs (SilverPush, Lisnr,
 * Shopkick, Signal360) to track users across devices.
 *
 * Uses AudioRecord at 48 kHz sample rate with FFT analysis.
 * Requires RECORD_AUDIO permission.
 */
@Singleton
class UltrasonicDetector @Inject constructor() {

    companion object {
        private const val TAG = "UltrasonicDetector"
        private const val SAMPLE_RATE = 48000
        private const val FFT_SIZE = 4096 // ~85ms window at 48kHz
        private const val MIN_FREQ_HZ = 17500f
        private const val MAX_FREQ_HZ = 22000f
        private const val DETECTION_THRESHOLD_DB = 12f // above noise floor
        private const val MIN_PERSISTENCE_FRAMES = 3 // ~255ms at 48kHz/4096
    }

    data class UltrasonicAlert(
        val frequencyHz: Float,
        val magnitudeDb: Float,
        val noiseFloorDb: Float,
        val snrDb: Float,
        val persistenceFrames: Int
    )

    /**
     * Start monitoring for ultrasonic beacons.
     * Emits UltrasonicAlert when a persistent narrowband peak is detected.
     * Runs until cancelled.
     */
    @Suppress("MissingPermission")
    fun startMonitoring(): Flow<UltrasonicAlert> = flow {
        val bufferSize = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(FFT_SIZE * 2)

        // Try UNPROCESSED source first (less AGC/noise reduction), fallback to MIC
        val audioSource = try {
            val src = MediaRecorder.AudioSource.UNPROCESSED
            val test = AudioRecord(src, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, bufferSize)
            if (test.state == AudioRecord.STATE_INITIALIZED) {
                test.release()
                src
            } else {
                test.release()
                MediaRecorder.AudioSource.MIC
            }
        } catch (_: Exception) {
            MediaRecorder.AudioSource.MIC
        }

        val recorder = AudioRecord(
            audioSource, SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufferSize
        )

        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord failed to initialize")
            recorder.release()
            return@flow
        }

        Log.i(TAG, "Ultrasonic monitoring started (${SAMPLE_RATE}Hz, source=$audioSource, FFT=$FFT_SIZE)")
        recorder.startRecording()

        val buffer = ShortArray(FFT_SIZE)
        val window = hannWindow(FFT_SIZE)
        val persistenceMap = mutableMapOf<Int, Int>() // bin -> consecutive frame count

        try {
            while (coroutineContext.isActive) {
                val read = recorder.read(buffer, 0, FFT_SIZE)
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
                            emit(UltrasonicAlert(
                                frequencyHz = freq,
                                magnitudeDb = magDb.toFloat(),
                                noiseFloorDb = noiseFloorDb.toFloat(),
                                snrDb = snr.toFloat(),
                                persistenceFrames = count
                            ))
                            Log.w(TAG, "ULTRASONIC BEACON: %.0fHz SNR=%.1fdB (%d frames)".format(freq, snr, count))
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
        } finally {
            recorder.stop()
            recorder.release()
            Log.i(TAG, "Ultrasonic monitoring stopped")
        }
    }.flowOn(Dispatchers.Default)

    private fun hannWindow(size: Int): DoubleArray {
        return DoubleArray(size) { 0.5 * (1.0 - cos(2.0 * Math.PI * it / (size - 1))) }
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
