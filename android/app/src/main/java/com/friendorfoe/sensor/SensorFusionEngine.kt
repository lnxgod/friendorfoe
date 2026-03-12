package com.friendorfoe.sensor

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Sensor Fusion Engine for computing device orientation from hardware sensors.
 *
 * Registers listeners for accelerometer, magnetometer, and gyroscope.
 * Applies a low-pass filter to accelerometer readings to reduce noise,
 * then uses [SensorManager.getRotationMatrix] and [SensorManager.getOrientation]
 * to compute azimuth (compass heading), pitch (elevation), and roll.
 *
 * The coordinate system is remapped via [SensorManager.remapCoordinateSystem]
 * with (X, Z) so that the orientation is correct when the phone is held upright
 * in portrait mode (as if pointing the camera at the sky).
 *
 * Architecture:
 * - Low-pass filter (alpha ~0.8) smooths accelerometer noise
 * - getRotationMatrix + remapCoordinateSystem for portrait/upright orientation
 * - Exposes orientation as a [StateFlow] for reactive consumers
 * - start()/stop() lifecycle methods to register/unregister sensor listeners
 *
 * Injected via Hilt; SensorManager is provided by [com.friendorfoe.di.SensorModule].
 */
@Singleton
class SensorFusionEngine @Inject constructor(
    private val sensorManager: SensorManager
) : SensorEventListener {

    companion object {
        /** Low-pass filter smoothing factor. Higher = more smoothing, slower response. */
        private const val LOW_PASS_ALPHA = 0.8f

        /** Sensor sampling period. SENSOR_DELAY_GAME (~20ms) is a good balance. */
        private const val SENSOR_DELAY = SensorManager.SENSOR_DELAY_GAME
    }

    private val _orientation = MutableStateFlow(DeviceOrientation())

    /** Current device orientation, updated as sensor data arrives. */
    val orientation: StateFlow<DeviceOrientation> = _orientation.asStateFlow()

    // Raw sensor data arrays
    private var gravity = FloatArray(3)
    private var geomagnetic = FloatArray(3)
    private var gyroscope = FloatArray(3)

    // Flag to track whether we have received at least one reading from each sensor
    private var hasGravity = false
    private var hasGeomagnetic = false

    // Reusable arrays to avoid allocation in the sensor callback hot path
    private val rotationMatrix = FloatArray(9)
    private val remappedRotationMatrix = FloatArray(9)
    private val orientationAngles = FloatArray(3)

    private var isRunning = false

    /**
     * Start listening to accelerometer, magnetometer, and gyroscope sensors.
     *
     * Call this in onResume() or when the AR view becomes active.
     * Safe to call multiple times; duplicate calls are ignored.
     */
    fun start() {
        if (isRunning) return
        isRunning = true

        val accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val magnetometer = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)
        val gyroscopeSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

        accelerometer?.let {
            sensorManager.registerListener(this, it, SENSOR_DELAY)
        }
        magnetometer?.let {
            sensorManager.registerListener(this, it, SENSOR_DELAY)
        }
        gyroscopeSensor?.let {
            sensorManager.registerListener(this, it, SENSOR_DELAY)
        }
    }

    /**
     * Stop listening to all sensors.
     *
     * Call this in onPause() or when the AR view is no longer visible
     * to conserve battery.
     */
    fun stop() {
        if (!isRunning) return
        isRunning = false
        sensorManager.unregisterListener(this)

        // Reset state so next start() begins fresh
        hasGravity = false
        hasGeomagnetic = false
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                gravity = lowPassFilter(event.values, gravity)
                hasGravity = true
            }
            Sensor.TYPE_MAGNETIC_FIELD -> {
                geomagnetic = lowPassFilter(event.values, geomagnetic)
                hasGeomagnetic = true
            }
            Sensor.TYPE_GYROSCOPE -> {
                // Store gyroscope data for potential future complementary filter use.
                // Currently orientation is derived from accelerometer + magnetometer
                // via getRotationMatrix, which is sufficient for our AR overlay use case.
                System.arraycopy(event.values, 0, gyroscope, 0, 3)
            }
        }

        // Only compute orientation when we have both required sensor inputs
        if (hasGravity && hasGeomagnetic) {
            updateOrientation()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
        // Could log accuracy changes for diagnostics; not critical for operation
    }

    /**
     * Compute device orientation from the current gravity and geomagnetic vectors.
     *
     * Uses remapCoordinateSystem(X, Z) to handle the phone being held upright
     * in portrait mode (the natural AR pose -- camera pointing forward/up at the sky).
     *
     * Without remapping, getOrientation() assumes the device is flat on a table.
     * Remapping X->X, Z->Y transforms the coordinate system so that:
     * - Azimuth: compass heading the camera is pointing at
     * - Pitch: elevation angle (positive = camera aimed above horizon)
     * - Roll: device tilt left/right
     */
    private fun updateOrientation() {
        val success = SensorManager.getRotationMatrix(
            rotationMatrix, null, gravity, geomagnetic
        )
        if (!success) return

        // Remap for portrait mode with phone held upright (camera pointing at sky).
        // SensorManager.AXIS_X keeps the X axis as-is.
        // SensorManager.AXIS_Z remaps the Y axis to Z, which makes pitch represent
        // elevation angle rather than table-tilt.
        SensorManager.remapCoordinateSystem(
            rotationMatrix,
            SensorManager.AXIS_X,
            SensorManager.AXIS_Z,
            remappedRotationMatrix
        )

        SensorManager.getOrientation(remappedRotationMatrix, orientationAngles)

        // orientationAngles[0] = azimuth in radians (-PI to PI)
        // orientationAngles[1] = pitch in radians (-PI/2 to PI/2)
        // orientationAngles[2] = roll in radians (-PI to PI)

        var azimuthDeg = Math.toDegrees(orientationAngles[0].toDouble()).toFloat()
        val pitchDeg = Math.toDegrees(orientationAngles[1].toDouble()).toFloat()
        val rollDeg = Math.toDegrees(orientationAngles[2].toDouble()).toFloat()

        // Normalize azimuth to 0-360 range
        if (azimuthDeg < 0) {
            azimuthDeg += 360f
        }

        _orientation.value = DeviceOrientation(
            azimuthDegrees = azimuthDeg,
            pitchDegrees = pitchDeg,
            rollDegrees = rollDeg
        )
    }

    /**
     * Low-pass filter to smooth sensor noise.
     *
     * output[i] = alpha * previous[i] + (1 - alpha) * input[i]
     *
     * With alpha = 0.8, the output retains 80% of the previous value and blends
     * in 20% of the new reading, providing strong smoothing that reduces jitter
     * while still tracking real movement within a few samples.
     *
     * @param input New raw sensor values
     * @param previous Previously filtered values
     * @return Filtered output values
     */
    private fun lowPassFilter(input: FloatArray, previous: FloatArray): FloatArray {
        val output = FloatArray(input.size)
        for (i in input.indices) {
            output[i] = LOW_PASS_ALPHA * previous[i] + (1f - LOW_PASS_ALPHA) * input[i]
        }
        return output
    }
}
