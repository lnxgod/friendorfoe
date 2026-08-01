package com.friendorfoe.detection

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.sqrt

data class MagneticSample(
    val totalMicroTesla: Float,
    val accuracy: Int,
)

sealed interface MagneticSensorEvent {
    data object Unavailable : MagneticSensorEvent
    data class Sample(val value: MagneticSample) : MagneticSensorEvent
    data class Failed(val message: String) : MagneticSensorEvent
}

/** Reads the phone magnetometer without assigning meaning to field strength. */
@Singleton
class EmfDetector @Inject constructor(
    private val sensorManager: SensorManager,
) {
    fun startMonitoring(): Flow<MagneticSensorEvent> = callbackFlow {
        val sensor = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)
        if (sensor == null) {
            trySend(MagneticSensorEvent.Unavailable)
            close()
            return@callbackFlow
        }

        var latestAccuracy = SensorManager.SENSOR_STATUS_UNRELIABLE
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                if (event.values.size < 3) {
                    trySend(MagneticSensorEvent.Failed("Could not read magnetometer"))
                    return
                }
                val x = event.values[0]
                val y = event.values[1]
                val z = event.values[2]
                val magnitude = sqrt(x * x + y * y + z * z)
                trySend(
                    MagneticSensorEvent.Sample(
                        MagneticSample(
                            totalMicroTesla = magnitude,
                            accuracy = latestAccuracy,
                        ),
                    ),
                )
            }

            override fun onAccuracyChanged(changedSensor: Sensor?, accuracy: Int) {
                if (changedSensor == null || changedSensor.type == Sensor.TYPE_MAGNETIC_FIELD) {
                    latestAccuracy = accuracy
                }
            }
        }

        val registered = runCatching {
            sensorManager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI)
        }.getOrElse {
            trySend(MagneticSensorEvent.Failed("Could not start magnetometer"))
            close()
            return@callbackFlow
        }
        if (!registered) {
            trySend(MagneticSensorEvent.Failed("Could not start magnetometer"))
            close()
            return@callbackFlow
        }

        awaitClose {
            sensorManager.unregisterListener(listener)
        }
    }
}
