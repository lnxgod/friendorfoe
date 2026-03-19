package com.friendorfoe.detection

import android.graphics.Bitmap
import android.graphics.ImageFormat
import android.graphics.YuvImage
import android.graphics.BitmapFactory
import android.graphics.Rect
import android.util.Log
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.objects.ObjectDetection
import com.google.mlkit.vision.objects.ObjectDetector
import com.google.mlkit.vision.objects.defaults.ObjectDetectorOptions
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicReference

/**
 * CameraX [ImageAnalysis.Analyzer] that runs ML Kit object detection on each frame.
 *
 * Produces [VisualDetection] objects with normalized coordinates (0-1).
 * Uses STREAM_MODE for real-time tracking with multiple object support
 * and classification enabled.
 *
 * Integrates [SkyObjectFilter] to score and classify detections as sky objects.
 * Retains the latest frame bitmap for tap-to-zoom functionality.
 *
 * Critical: Always closes [ImageProxy] in the completion listener to avoid
 * blocking the next camera frame.
 */
class VisualDetectionAnalyzer(
    private val skyObjectFilter: SkyObjectFilter = SkyObjectFilter()
) : ImageAnalysis.Analyzer {

    companion object {
        private const val TAG = "VisualDetection"
        /** Minimum label confidence to keep a detection (detections with no labels are kept as "unknown") */
        private const val MIN_CONFIDENCE = 0.3f
    }

    private val _detections = MutableStateFlow<List<VisualDetection>>(emptyList())
    val detections: StateFlow<List<VisualDetection>> = _detections.asStateFlow()

    private val _scoredDetections = MutableStateFlow<List<ScoredVisualDetection>>(emptyList())
    val scoredDetections: StateFlow<List<ScoredVisualDetection>> = _scoredDetections.asStateFlow()

    private val _strobeDetections = MutableStateFlow<List<StrobeDetection>>(emptyList())
    /** Strobe detections from nighttime LED/strobe pattern analysis. */
    val strobeDetections: StateFlow<List<StrobeDetection>> = _strobeDetections.asStateFlow()

    /** Latest frame bitmap retained for tap-to-zoom (thread-safe) */
    private val lastFrame = AtomicReference<Bitmap?>(null)

    /** Weather-based confidence multiplier, updated from ArViewModel */
    @Volatile
    var confidenceMultiplier: Float = 1.0f

    /** Current device pitch in degrees, updated from ArViewModel. Positive = above horizon. */
    @Volatile
    var currentPitch: Float = 0f

    /** Whether the scene is currently dark enough for strobe detection */
    @Volatile
    var isDarkMode: Boolean = false
        private set

    private val strobeDetector = StrobePatternDetector()

    private val detector: ObjectDetector = run {
        val options = ObjectDetectorOptions.Builder()
            .setDetectorMode(ObjectDetectorOptions.STREAM_MODE)
            .enableMultipleObjects()
            .enableClassification()
            .build()
        ObjectDetection.getClient(options)
    }

    @androidx.camera.core.ExperimentalGetImage
    override fun analyze(imageProxy: ImageProxy) {
        val mediaImage = imageProxy.image
        if (mediaImage == null) {
            imageProxy.close()
            return
        }

        try {
        // Capture frame bitmap before ML Kit processes it
        try {
            val bitmap = imageProxy.toBitmapSafe()
            if (bitmap != null) {
                val oldBitmap = lastFrame.getAndSet(bitmap)
                if (oldBitmap != null && oldBitmap != bitmap) {
                    oldBitmap.recycle()
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to capture frame bitmap", e)
        }

        // --- Ambient light check: extract Y (luminance) plane for brightness ---
        val grayscaleData = try {
            if (imageProxy.planes.isEmpty()) null
            else {
                val yPlane = imageProxy.planes[0]
                val yBuffer = yPlane.buffer.duplicate()
                val yBytes = ByteArray(yBuffer.remaining())
                yBuffer.get(yBytes)
                yBytes
            }
        } catch (e: Exception) {
            null
        }

        val ambientBrightness = if (grayscaleData != null) {
            strobeDetector.measureAmbientBrightness(grayscaleData)
        } else {
            255f // Assume bright if we can't measure
        }
        isDarkMode = ambientBrightness < StrobePatternDetector.DARK_THRESHOLD

        // --- Dark mode: run strobe detection alongside ML Kit ---
        if (isDarkMode && grayscaleData != null) {
            val strobes = strobeDetector.processFrame(
                grayscale = grayscaleData,
                width = imageProxy.width,
                height = imageProxy.height,
                timestampMs = System.currentTimeMillis(),
                devicePitchDegrees = currentPitch
            )
            _strobeDetections.value = strobes

            if (strobes.isNotEmpty()) {
                Log.d(TAG, "Dark mode: ${strobes.size} strobe(s) detected " +
                        "(ambient brightness=${"%.0f".format(ambientBrightness)})")
            }
        } else {
            // Clear strobe detections in daylight
            if (_strobeDetections.value.isNotEmpty()) {
                _strobeDetections.value = emptyList()
            }
        }

        val inputImage = InputImage.fromMediaImage(
            mediaImage,
            imageProxy.imageInfo.rotationDegrees
        )

        val imageWidth = inputImage.width.toFloat()
        val imageHeight = inputImage.height.toFloat()

        detector.process(inputImage)
            .addOnSuccessListener { detectedObjects ->
                val rawDetections = detectedObjects.map { obj ->
                    val box = obj.boundingBox
                    val centerX = (box.left + box.right) / 2f / imageWidth
                    val centerY = (box.top + box.bottom) / 2f / imageHeight
                    val width = box.width().toFloat() / imageWidth
                    val height = box.height().toFloat() / imageHeight

                    val labelTexts = obj.labels.map { it.text }
                    val labelConfs = obj.labels.map { it.confidence }

                    VisualDetection(
                        trackingId = obj.trackingId,
                        centerX = centerX,
                        centerY = centerY,
                        width = width,
                        height = height,
                        labels = labelTexts,
                        labelConfidences = labelConfs,
                        timestampMs = System.currentTimeMillis()
                    )
                }.filter { detection ->
                    // Keep detections with no labels (unknown objects) or with at least one label above threshold
                    detection.labels.isEmpty() ||
                        detection.labelConfidences.any { it >= MIN_CONFIDENCE }
                }

                // Run sky object filter to score and classify (pass strobe data for dark mode)
                val currentStrobes = if (isDarkMode) _strobeDetections.value else emptyList()
                val scored = skyObjectFilter.filter(rawDetections, confidenceMultiplier, currentStrobes, currentPitch)
                _scoredDetections.value = scored

                // Enrich detections with scores and classifications
                val enrichedResults = scored.map { s ->
                    s.detection.copy(
                        skyScore = s.skyScore,
                        motionScore = s.motionScore,
                        visualClassification = s.classification,
                        strobeConfirmed = s.detection.labels.contains("strobe") ||
                            (isDarkMode && s.skyScore > s.detection.skyScore)
                    )
                }

                _detections.value = enrichedResults
                if (enrichedResults.isNotEmpty()) {
                    Log.d(TAG, "Detected ${enrichedResults.size} objects (${scored.count { it.classification != VisualClassification.LIKELY_STATIC }} non-static)")
                }
            }
            .addOnFailureListener { e ->
                Log.w(TAG, "ML Kit detection failed", e)
            }
            .addOnCompleteListener {
                // Critical: always close to unblock next frame
                imageProxy.close()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception before ML Kit processing, closing imageProxy", e)
            imageProxy.close()
        }
    }

    /** Get the most recently captured camera frame (thread-safe). */
    fun getLastFrame(): Bitmap? = lastFrame.get()

    fun close() {
        detector.close()
        skyObjectFilter.reset()
        strobeDetector.reset()
        lastFrame.getAndSet(null)?.recycle()
        Log.d(TAG, "Visual detector closed")
    }

    /**
     * Convert ImageProxy to Bitmap using YUV to JPEG conversion.
     * Returns null if conversion fails.
     */
    private fun ImageProxy.toBitmapSafe(): Bitmap? {
        return try {
            if (planes.size < 3) return null
            val yBuffer = planes[0].buffer
            val uBuffer = planes[1].buffer
            val vBuffer = planes[2].buffer

            val ySize = yBuffer.remaining()
            val uSize = uBuffer.remaining()
            val vSize = vBuffer.remaining()

            val nv21 = ByteArray(ySize + uSize + vSize)
            yBuffer.get(nv21, 0, ySize)
            vBuffer.get(nv21, ySize, vSize)
            uBuffer.get(nv21, ySize + vSize, uSize)

            val yuvImage = YuvImage(nv21, ImageFormat.NV21, width, height, null)
            val out = ByteArrayOutputStream()
            yuvImage.compressToJpeg(Rect(0, 0, width, height), 80, out)
            val imageBytes = out.toByteArray()
            BitmapFactory.decodeByteArray(imageBytes, 0, imageBytes.size)
        } catch (e: Exception) {
            null
        }
    }
}
