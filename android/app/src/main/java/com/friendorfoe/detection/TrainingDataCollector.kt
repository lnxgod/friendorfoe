package com.friendorfoe.detection

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Auto-collects labeled training data from ADS-B-correlated visual detections.
 *
 * When a visual detection matches an ADS-B aircraft, saves the cropped image
 * labeled with the aircraft type. This builds a custom training dataset
 * over time for future model training.
 *
 * Data is saved to: app-specific-dir/training_data/{label}/{timestamp}.jpg
 */
@Singleton
class TrainingDataCollector @Inject constructor() {

    companion object {
        private const val TAG = "TrainingData"
        private const val MAX_IMAGES_PER_LABEL = 200
        private const val MIN_CROP_SIZE = 32 // Ignore crops smaller than 32x32
    }

    private var saveDir: File? = null
    private var totalSaved = 0

    fun initialize(context: Context) {
        saveDir = File(context.filesDir, "training_data")
        saveDir?.mkdirs()
        Log.i(TAG, "Training data dir: ${saveDir?.absolutePath}")
    }

    /**
     * Save a cropped detection image with its label.
     * @param crop The cropped bitmap of the detected object
     * @param label The aircraft type label (e.g., "B738", "HELI", "DRONE")
     * @param confidence The detection confidence (only save high-confidence crops)
     */
    fun saveLabeledCrop(crop: Bitmap, label: String, confidence: Float) {
        if (saveDir == null) return
        if (crop.width < MIN_CROP_SIZE || crop.height < MIN_CROP_SIZE) return
        if (confidence < 0.7f) return // Only save high-confidence matches

        val safeLabel = label.replace(Regex("[^a-zA-Z0-9_]"), "_").uppercase()
        val labelDir = File(saveDir, safeLabel)
        labelDir.mkdirs()

        // Check count limit
        val existing = labelDir.listFiles()?.size ?: 0
        if (existing >= MAX_IMAGES_PER_LABEL) return

        val timestamp = System.currentTimeMillis()
        val file = File(labelDir, "${timestamp}.jpg")

        try {
            FileOutputStream(file).use { out ->
                crop.compress(Bitmap.CompressFormat.JPEG, 85, out)
            }
            totalSaved++
            if (totalSaved % 10 == 0) {
                Log.i(TAG, "Saved $totalSaved training images (latest: $safeLabel)")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to save training image: ${e.message}")
        }
    }

    /** Get count of saved images per label */
    fun getStats(): Map<String, Int> {
        val dir = saveDir ?: return emptyMap()
        return dir.listFiles()?.filter { it.isDirectory }?.associate {
            it.name to (it.listFiles()?.size ?: 0)
        } ?: emptyMap()
    }

    /** Total saved images */
    fun getTotalCount(): Int = totalSaved
}
