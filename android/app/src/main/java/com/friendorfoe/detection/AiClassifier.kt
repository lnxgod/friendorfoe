package com.friendorfoe.detection

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.label.ImageLabeler
import com.google.mlkit.vision.label.ImageLabeling
import com.google.mlkit.vision.label.defaults.ImageLabelerOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/**
 * On-device AI classifier using ML Kit Image Labeling.
 *
 * Classifies cropped images of detected objects to determine if they're
 * aircraft, helicopters, drones, birds, or other objects. Uses Google's
 * bundled image labeling model — no cloud needed, no API key.
 *
 * Also collects labeled training data when ADS-B correlation confirms
 * the aircraft type, building a custom dataset for future model training.
 */
@Singleton
class AiClassifier @Inject constructor() {

    companion object {
        private const val TAG = "AiClassifier"
        private const val MIN_CONFIDENCE = 0.3f
    }

    private var labeler: ImageLabeler? = null
    private var available = false

    /**
     * Initialize the on-device ML model.
     */
    fun initialize(context: Context): Boolean {
        return try {
            val options = ImageLabelerOptions.Builder()
                .setConfidenceThreshold(MIN_CONFIDENCE)
                .build()
            labeler = ImageLabeling.getClient(options)
            available = true
            Log.i(TAG, "ML Kit Image Labeler initialized")
            true
        } catch (e: Exception) {
            Log.w(TAG, "ML Kit Image Labeler failed: ${e.message}")
            false
        }
    }

    /**
     * Classify a cropped image and return all labels with confidence.
     */
    suspend fun classifyImage(bitmap: Bitmap): List<Pair<String, Float>> {
        val lab = labeler ?: return emptyList()

        return withContext(Dispatchers.IO) {
            try {
                val image = InputImage.fromBitmap(bitmap, 0)
                suspendCancellableCoroutine { cont ->
                    lab.process(image)
                        .addOnSuccessListener { labels ->
                            val results = labels.map { it.text to it.confidence }
                            Log.d(TAG, "Labels: ${results.joinToString { "${it.first}(${String.format("%.0f%%", it.second * 100)})" }}")
                            cont.resume(results)
                        }
                        .addOnFailureListener { e ->
                            Log.w(TAG, "Labeling failed: ${e.message}")
                            cont.resume(emptyList())
                        }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Classification error: ${e.message}")
                emptyList()
            }
        }
    }

    /**
     * Classify a cropped aircraft image and extract the type.
     */
    suspend fun classifyAircraft(bitmap: Bitmap): AircraftClassification {
        val labels = classifyImage(bitmap)
        if (labels.isEmpty()) return AircraftClassification.UNKNOWN

        // Check labels for aircraft-related keywords
        for ((label, confidence) in labels) {
            val lower = label.lowercase()
            when {
                lower.contains("helicopter") || lower.contains("rotorcraft") ->
                    return AircraftClassification.HELICOPTER
                lower.contains("drone") || lower.contains("quadcopter") ->
                    return AircraftClassification.DRONE
                lower.contains("fighter") || lower.contains("military") ->
                    return AircraftClassification.MILITARY
                lower.contains("airliner") || lower.contains("airplane") || lower.contains("aircraft") ->
                    return if (confidence > 0.6f) AircraftClassification.COMMERCIAL
                    else AircraftClassification.AIRCRAFT
                lower.contains("bird") ->
                    return AircraftClassification.BIRD
                lower.contains("vehicle") || lower.contains("transport") ->
                    return AircraftClassification.AIRCRAFT
                lower.contains("sky") || lower.contains("cloud") ->
                    continue // Skip non-object labels
            }
        }
        return AircraftClassification.UNKNOWN
    }

    fun isAvailable(): Boolean = available

    fun shutdown() {
        labeler?.close()
        labeler = null
        available = false
    }
}

enum class AircraftClassification(val label: String) {
    COMMERCIAL("Commercial Jet"),
    MILITARY("Military"),
    HELICOPTER("Helicopter"),
    DRONE("Drone"),
    GENERAL_AVIATION("Light Aircraft"),
    AIRCRAFT("Aircraft"),
    BIRD("Bird"),
    UNKNOWN("Unknown")
}
