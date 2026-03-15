package com.friendorfoe.sensor

import android.util.Log
import com.google.ar.core.Session
import com.google.ar.core.TrackingState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.atan2

/**
 * Provides device orientation anchored to compass north using ARCore pose tracking.
 *
 * On first TRACKING frame, captures the compass azimuth from [SensorFusionEngine]
 * and the ARCore yaw, establishing an anchor. Subsequent frames compute absolute
 * azimuth as: initialCompassAzimuth + (currentArCoreYaw - initialArCoreYaw).
 *
 * Pitch and roll come directly from the ARCore pose quaternion.
 *
 * When tracking transitions false→true, the anchor is automatically re-established.
 *
 * Note: ARCore tracking degrades when pointed at featureless sky. This is a
 * supplemental orientation source — compass-math continues as fallback.
 */
@Singleton
class ArCoreOrientationProvider @Inject constructor() {

    companion object {
        private const val TAG = "ArCoreOrientation"
    }

    private var session: Session? = null

    private val _arOrientation = MutableStateFlow<DeviceOrientation?>(null)
    val arOrientation: StateFlow<DeviceOrientation?> = _arOrientation.asStateFlow()

    private val _isTracking = MutableStateFlow(false)
    val isTracking: StateFlow<Boolean> = _isTracking.asStateFlow()

    // Anchor state: captured on first TRACKING frame
    private var initialCompassAzimuth: Float? = null
    private var initialArCoreYaw: Float? = null
    private var wasTracking = false

    fun setSession(session: Session) {
        this.session = session
        Log.d(TAG, "ARCore session set")
    }

    /**
     * Called each frame (~30fps) to update orientation from ARCore pose.
     *
     * @param currentCompassAzimuth Current compass azimuth from SensorFusionEngine,
     *        used to anchor ARCore yaw to magnetic north on first tracking frame.
     * @param currentCompassPitch Current compass pitch from SensorFusionEngine,
     *        used to verify ARCore pitch sign matches (positive=up convention).
     */
    fun update(currentCompassAzimuth: Float, currentCompassPitch: Float = 0f) {
        val sess = session ?: return

        try {
            val frame = sess.update()
            val camera = frame.camera
            val tracking = camera.trackingState == TrackingState.TRACKING
            _isTracking.value = tracking

            if (!tracking) {
                _arOrientation.value = null
                wasTracking = false
                return
            }

            // Extract yaw/pitch/roll from ARCore pose quaternion
            val pose = camera.displayOrientedPose
            val (yaw, pitch, roll) = quaternionToEuler(
                pose.qx(), pose.qy(), pose.qz(), pose.qw()
            )

            // Re-anchor on tracking transition (false→true)
            if (!wasTracking) {
                initialCompassAzimuth = currentCompassAzimuth
                initialArCoreYaw = yaw
                wasTracking = true
                Log.d(TAG, "Anchored: compass=$currentCompassAzimuth, arYaw=$yaw")
            }

            val anchorCompass = initialCompassAzimuth ?: return
            val anchorYaw = initialArCoreYaw ?: return

            // Compute absolute azimuth: compass anchor + delta from ARCore
            var absoluteAzimuth = anchorCompass + (yaw - anchorYaw)
            // Normalize to 0-360
            absoluteAzimuth = ((absoluteAzimuth % 360f) + 360f) % 360f

            // Log pitch comparison to verify ARCore sign matches compass convention
            // ARCore displayOrientedPose: positive pitch = looking up (matches our convention)
            if (android.os.SystemClock.elapsedRealtime() % 3000 < 100) {
                Log.d(TAG, "Pitch comparison: compass=$currentCompassPitch, arcore=$pitch")
            }

            _arOrientation.value = DeviceOrientation(
                azimuthDegrees = absoluteAzimuth,
                pitchDegrees = pitch,
                rollDegrees = roll
            )
        } catch (e: Exception) {
            Log.w(TAG, "Error updating ARCore orientation", e)
            _arOrientation.value = null
            _isTracking.value = false
        }
    }

    fun stop() {
        _arOrientation.value = null
        _isTracking.value = false
        wasTracking = false
        initialCompassAzimuth = null
        initialArCoreYaw = null
    }

    /**
     * Convert ARCore pose quaternion to Euler angles (yaw, pitch, roll) in degrees.
     *
     * ARCore coordinate system: X right, Y up, Z toward user.
     * Returns yaw (rotation around Y), pitch (rotation around X), roll (rotation around Z).
     */
    private fun quaternionToEuler(qx: Float, qy: Float, qz: Float, qw: Float): Triple<Float, Float, Float> {
        // Yaw (azimuth) - rotation around Y axis
        val sinYaw = 2.0 * (qw * qy - qz * qx)
        val cosYaw = 1.0 - 2.0 * (qx * qx + qy * qy)
        val yaw = Math.toDegrees(atan2(sinYaw, cosYaw)).toFloat()

        // Pitch - rotation around X axis
        val sinPitch = 2.0 * (qw * qx + qy * qz)
        val cosPitch = 1.0 - 2.0 * (qx * qx + qz * qz)
        val pitch = Math.toDegrees(atan2(sinPitch, cosPitch)).toFloat()

        // Roll - rotation around Z axis
        val sinRollCosPitch = 2.0 * (qw * qz + qx * qy)
        val cosRollCosPitch = 1.0 - 2.0 * (qy * qy + qz * qz)
        val roll = Math.toDegrees(atan2(sinRollCosPitch, cosRollCosPitch)).toFloat()

        return Triple(yaw, pitch, roll)
    }
}
