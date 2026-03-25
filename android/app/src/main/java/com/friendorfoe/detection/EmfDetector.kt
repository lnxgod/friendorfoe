package com.friendorfoe.detection

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.util.Log
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.sqrt

/**
 * Electromagnetic field detector using the phone's magnetometer.
 *
 * Detects anomalous magnetic fields from hidden electronics (cameras,
 * recording devices, transmitters) at close range (1-5cm).
 *
 * Normal ambient: ~25-65 µT (Earth's magnetic field)
 * Electronics: spike to 100-500+ µT at close range
 */
@Singleton
class EmfDetector @Inject constructor(
    private val sensorManager: SensorManager
) {
    companion object {
        private const val TAG = "EmfDetector"
        const val THRESHOLD_LOW = 80f    // µT — slightly elevated
        const val THRESHOLD_MEDIUM = 150f // µT — definitely electronics nearby
        const val THRESHOLD_HIGH = 300f   // µT — very strong source (motor, speaker, camera)
    }

    data class EmfReading(
        val magnitudeUt: Float,  // Total magnitude in microtesla
        val x: Float,            // X component µT
        val y: Float,            // Y component µT
        val z: Float,            // Z component µT
        val level: EmfLevel
    )

    enum class EmfLevel {
        NORMAL,    // < 80 µT
        LOW,       // 80-150 µT — slightly elevated
        MEDIUM,    // 150-300 µT — electronics nearby
        HIGH       // > 300 µT — strong source
    }

    /**
     * Start EMF monitoring. Emits readings at sensor rate.
     * Returns a Flow of EmfReading with magnitude and threat level.
     */
    fun startMonitoring(): Flow<EmfReading> = callbackFlow {
        val sensor = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)
        if (sensor == null) {
            Log.w(TAG, "Magnetometer not available")
            close()
            return@callbackFlow
        }

        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                val x = event.values[0]
                val y = event.values[1]
                val z = event.values[2]
                val magnitude = sqrt(x * x + y * y + z * z)

                val level = when {
                    magnitude >= THRESHOLD_HIGH -> EmfLevel.HIGH
                    magnitude >= THRESHOLD_MEDIUM -> EmfLevel.MEDIUM
                    magnitude >= THRESHOLD_LOW -> EmfLevel.LOW
                    else -> EmfLevel.NORMAL
                }

                trySend(EmfReading(magnitude, x, y, z, level))
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }

        sensorManager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI)
        Log.i(TAG, "EMF monitoring started")

        awaitClose {
            sensorManager.unregisterListener(listener)
            Log.i(TAG, "EMF monitoring stopped")
        }
    }
}
