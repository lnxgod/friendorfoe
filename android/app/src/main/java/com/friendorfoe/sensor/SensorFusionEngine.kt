package com.friendorfoe.sensor

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.util.Log
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
        private const val TAG = "SensorFusionEngine"

        /** Low-pass filter smoothing factor for raw accel/mag input. */
        private const val LOW_PASS_ALPHA = 0.65f

        /** Output smoothing factor applied to final orientation angles.
         *  Reduces jitter from all sensor sources including rotation vector. */
        private const val OUTPUT_SMOOTH_ALPHA = 0.25f

        /** Sensor sampling period. SENSOR_DELAY_GAME (~20ms) is a good balance. */
        private const val SENSOR_DELAY = SensorManager.SENSOR_DELAY_GAME
    }

    private val _orientation = MutableStateFlow(DeviceOrientation())

    /**
     * Current magnetometer accuracy level, tracked from [onAccuracyChanged].
     *
     * Values correspond to [SensorManager] accuracy constants:
     * - [SensorManager.SENSOR_STATUS_ACCURACY_HIGH] (3)
     * - [SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM] (2)
     * - [SensorManager.SENSOR_STATUS_ACCURACY_LOW] (1)
     * - [SensorManager.SENSOR_STATUS_UNRELIABLE] (0)
     *
     * When accuracy is LOW or UNRELIABLE, the UI should prompt the user
     * to calibrate the compass by waving the phone in a figure-8 pattern.
     */
    private val _sensorAccuracy = MutableStateFlow(SensorManager.SENSOR_STATUS_ACCURACY_HIGH)
    val sensorAccuracy: StateFlow<Int> = _sensorAccuracy.asStateFlow()

    /** Current sensor delay used for registration. */
    private var currentSensorDelay: Int = SENSOR_DELAY

    /** Current device orientation, updated as sensor data arrives. */
    val orientation: StateFlow<DeviceOrientation> = _orientation.asStateFlow()

    // Raw sensor data arrays
    private var gravity = FloatArray(3)
    private var geomagnetic = FloatArray(3)

    // Flag to track whether we have received at least one reading from each sensor
    private var hasGravity = false
    private var hasGeomagnetic = false

    /** Whether the device has a gyroscope-based rotation vector sensor. */
    private var hasRotationVector = false

    // Reusable arrays to avoid allocation in the sensor callback hot path
    private val rotationMatrix = FloatArray(9)
    private val remappedRotationMatrix = FloatArray(9)
    private val orientationAngles = FloatArray(3)

    // Output smoothing state (applied after orientation computation)
    private var smoothedAzimuth = Float.NaN
    private var smoothedPitch = Float.NaN
    private var smoothedRoll = Float.NaN

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

        // 1. Prefer TYPE_ROTATION_VECTOR (accel+gyro+mag fusion: smooth AND compass-calibrated).
        // This gives azimuth aligned to magnetic north, unlike TYPE_GAME_ROTATION_VECTOR.
        val rotationVector = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        if (rotationVector != null) {
            sensorManager.registerListener(this, rotationVector, currentSensorDelay)
            hasRotationVector = true
            Log.d(TAG, "Using TYPE_ROTATION_VECTOR for orientation (compass-calibrated)")
        }

        // 2. Fallback: TYPE_GAME_ROTATION_VECTOR (no compass, but smooth)
        if (!hasRotationVector) {
            val gameRotation = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
            if (gameRotation != null) {
                sensorManager.registerListener(this, gameRotation, currentSensorDelay)
                hasRotationVector = true
                Log.d(TAG, "Using TYPE_GAME_ROTATION_VECTOR fallback for orientation")
            }
        }

        // 3. Always register accel+mag as last fallback (no gyro devices)
        val accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val magnetometer = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)

        accelerometer?.let {
            sensorManager.registerListener(this, it, currentSensorDelay)
        }
        magnetometer?.let {
            sensorManager.registerListener(this, it, currentSensorDelay)
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
        hasRotationVector = false
        smoothedAzimuth = Float.NaN
        smoothedPitch = Float.NaN
        smoothedRoll = Float.NaN
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ROTATION_VECTOR, Sensor.TYPE_GAME_ROTATION_VECTOR -> {
                SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values)
                updateOrientation()
            }
            Sensor.TYPE_ACCELEROMETER -> {
                gravity = lowPassFilter(event.values, gravity)
                hasGravity = true
            }
            Sensor.TYPE_MAGNETIC_FIELD -> {
                geomagnetic = lowPassFilter(event.values, geomagnetic)
                hasGeomagnetic = true
            }
        }

        // Fallback: use accel+mag when rotation vector is not available
        if (!hasRotationVector && hasGravity && hasGeomagnetic) {
            updateOrientation()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
        if (sensor?.type == Sensor.TYPE_MAGNETIC_FIELD) {
            _sensorAccuracy.value = accuracy
            Log.d(TAG, "Magnetometer accuracy changed: $accuracy")
        }
    }

    /**
     * Change the sensor sampling delay.
     *
     * Use [SensorManager.SENSOR_DELAY_GAME] (~20ms) when the app is in the foreground
     * and actively displaying the AR overlay. Use [SensorManager.SENSOR_DELAY_NORMAL]
     * (~200ms) when the app is backgrounded to reduce battery drain.
     *
     * If sensors are currently running, they are re-registered with the new delay.
     *
     * @param delay One of [SensorManager.SENSOR_DELAY_GAME], [SensorManager.SENSOR_DELAY_NORMAL],
     *              [SensorManager.SENSOR_DELAY_UI], or [SensorManager.SENSOR_DELAY_FASTEST]
     */
    fun setSensorDelay(delay: Int) {
        if (delay == currentSensorDelay) return
        currentSensorDelay = delay
        Log.d(TAG, "Sensor delay changed to $delay")

        // Re-register listeners with the new delay if currently running
        if (isRunning) {
            sensorManager.unregisterListener(this)

            // Re-register rotation vector with same preference order
            var reRegisteredRotation = false
            val rotationVector = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
            if (rotationVector != null) {
                sensorManager.registerListener(this, rotationVector, currentSensorDelay)
                reRegisteredRotation = true
            }
            if (!reRegisteredRotation) {
                val gameRotation = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
                gameRotation?.let {
                    sensorManager.registerListener(this, it, currentSensorDelay)
                }
            }

            val accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            val magnetometer = sensorManager.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)

            accelerometer?.let {
                sensorManager.registerListener(this, it, currentSensorDelay)
            }
            magnetometer?.let {
                sensorManager.registerListener(this, it, currentSensorDelay)
            }
        }
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
        if (!hasRotationVector) {
            // Fallback: compute rotation matrix from accel+mag
            val success = SensorManager.getRotationMatrix(
                rotationMatrix, null, gravity, geomagnetic
            )
            if (!success) return
        }
        // rotationMatrix is already set by getRotationMatrixFromVector when using rotation vector

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
        val pitchDeg = -Math.toDegrees(orientationAngles[1].toDouble()).toFloat()
        val rollDeg = Math.toDegrees(orientationAngles[2].toDouble()).toFloat()

        // Normalize azimuth to 0-360 range
        if (azimuthDeg < 0) {
            azimuthDeg += 360f
        }

        // Apply output smoothing to reduce jitter from all sensor sources.
        // For azimuth, use circular interpolation to handle the 0/360 wrap.
        if (smoothedAzimuth.isNaN()) {
            smoothedAzimuth = azimuthDeg
            smoothedPitch = pitchDeg
            smoothedRoll = rollDeg
        } else {
            smoothedAzimuth = circularLerp(smoothedAzimuth, azimuthDeg, OUTPUT_SMOOTH_ALPHA)
            smoothedPitch += OUTPUT_SMOOTH_ALPHA * (pitchDeg - smoothedPitch)
            smoothedRoll += OUTPUT_SMOOTH_ALPHA * (rollDeg - smoothedRoll)
        }

        _orientation.value = DeviceOrientation(
            azimuthDegrees = smoothedAzimuth,
            pitchDegrees = smoothedPitch,
            rollDegrees = smoothedRoll
        )
    }

    /**
     * Circular linear interpolation for angular values (handles 0/360 wrap).
     * Interpolates from [current] toward [target] by [alpha] fraction.
     */
    private fun circularLerp(current: Float, target: Float, alpha: Float): Float {
        var diff = target - current
        if (diff > 180f) diff -= 360f
        if (diff < -180f) diff += 360f
        var result = current + alpha * diff
        if (result < 0f) result += 360f
        if (result >= 360f) result -= 360f
        return result
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
