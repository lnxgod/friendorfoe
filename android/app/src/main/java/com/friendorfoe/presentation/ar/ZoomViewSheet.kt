package com.friendorfoe.presentation.ar

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import android.widget.Toast
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.friendorfoe.detection.AlertLevel
import com.friendorfoe.detection.ClassifiedVisualDetection
import com.friendorfoe.detection.VisualClassification
import com.friendorfoe.detection.VisualDetection
import kotlinx.coroutines.delay
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Bottom sheet that shows a zoomed crop of a visual detection from the camera frame.
 * Supports pinch-to-zoom, pan, and photo save to device gallery.
 * Auto-refreshes every 300ms while open.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ZoomViewSheet(
    detection: VisualDetection,
    classified: ClassifiedVisualDetection?,
    getFrame: () -> Bitmap?,
    currentZoomRatio: Float = 1.0f,
    maxZoomRatio: Float = 1.0f,
    onZoomChange: (Float) -> Unit = {},
    onDismiss: () -> Unit
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val context = LocalContext.current

    // Crop state that auto-refreshes
    var croppedBitmap by remember { mutableStateOf<Bitmap?>(null) }
    var fullFrame by remember { mutableStateOf<Bitmap?>(null) }

    // Save state
    var saveState by remember { mutableStateOf(SaveState.IDLE) }
    var autoSaved by remember { mutableStateOf(false) }

    // Pinch-to-zoom state — start at 2x for better initial view
    var scale by remember { mutableFloatStateOf(2f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    val transformableState = rememberTransformableState { zoomChange, offsetChange, _ ->
        val newScale = (scale * zoomChange).coerceIn(1f, 8f)
        // Constrain pan so the image doesn't leave the viewport
        val maxOffsetX = (newScale - 1f) * 200f
        val maxOffsetY = (newScale - 1f) * 150f
        val newOffset = offset + offsetChange * scale
        scale = newScale
        offset = Offset(
            newOffset.x.coerceIn(-maxOffsetX, maxOffsetX),
            newOffset.y.coerceIn(-maxOffsetY, maxOffsetY)
        )
    }

    val classification = classified?.classification ?: detection.visualClassification

    // Auto-refresh crop every 300ms for smoother updates
    LaunchedEffect(detection.trackingId) {
        while (true) {
            val frame = getFrame()
            if (frame != null) {
                fullFrame = frame
                croppedBitmap = cropDetection(frame, detection)
            }
            delay(300L)
        }
    }

    // Auto-save first good frame for identified objects (LIKELY_DRONE or LIKELY_AIRCRAFT)
    LaunchedEffect(croppedBitmap, classification) {
        if (!autoSaved && croppedBitmap != null && fullFrame != null) {
            val shouldAutoSave = classification == VisualClassification.LIKELY_DRONE ||
                    classification == VisualClassification.LIKELY_AIRCRAFT
            if (shouldAutoSave) {
                autoSaved = true
                saveDetectionPhotos(
                    context = context,
                    cropped = croppedBitmap!!,
                    fullFrame = fullFrame!!,
                    detection = detection,
                    classificationLabel = classificationLabel(classification)
                )
            }
        }
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp)
        ) {
            // Header with classification
            val alertLevel = classified?.alertLevel ?: AlertLevel.NORMAL
            val headerColor = classificationColor(classification)

            Text(
                text = classificationLabel(classification),
                color = headerColor,
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(bottom = 4.dp)
            )

            // Alert level badge
            if (alertLevel != AlertLevel.NORMAL) {
                val alertColor = if (alertLevel == AlertLevel.ALERT) Color(0xFFF44336) else Color(0xFFFF9800)
                val alertText = if (alertLevel == AlertLevel.ALERT) "ALERT" else "INTEREST"
                Text(
                    text = alertText,
                    color = alertColor,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .background(alertColor.copy(alpha = 0.2f), RoundedCornerShape(4.dp))
                        .padding(horizontal = 8.dp, vertical = 2.dp)
                )
                Spacer(modifier = Modifier.height(4.dp))
            }

            // Persistence info
            classified?.let {
                Text(
                    text = "Tracked for %.1fs".format(it.persistenceSeconds),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Cropped image with pinch-to-zoom — taller viewport
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 250.dp, max = 400.dp)
                    .background(Color.Black, RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center
            ) {
                val bitmap = croppedBitmap
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap.asImageBitmap(),
                        contentDescription = "Zoomed detection",
                        contentScale = ContentScale.Fit,
                        modifier = Modifier
                            .fillMaxWidth()
                            .graphicsLayer(
                                scaleX = scale,
                                scaleY = scale,
                                translationX = offset.x,
                                translationY = offset.y
                            )
                            .transformable(state = transformableState)
                    )

                    // Zoom level indicator
                    Text(
                        text = "%.1fx".format(scale),
                        color = Color.White.copy(alpha = 0.6f),
                        fontSize = 11.sp,
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(8.dp)
                            .background(Color.Black.copy(alpha = 0.5f), RoundedCornerShape(4.dp))
                            .padding(horizontal = 6.dp, vertical = 2.dp)
                    )
                } else {
                    Text(
                        text = "Capturing...",
                        color = Color.White.copy(alpha = 0.5f),
                        fontSize = 14.sp
                    )
                }
            }

            // Hardware zoom slider (only show if camera supports zoom > 1x)
            if (maxZoomRatio > 1.1f) {
                Spacer(modifier = Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        text = "Zoom",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodySmall
                    )
                    Slider(
                        value = currentZoomRatio,
                        onValueChange = { onZoomChange(it) },
                        valueRange = 1.0f..maxZoomRatio,
                        colors = SliderDefaults.colors(
                            thumbColor = MaterialTheme.colorScheme.primary,
                            activeTrackColor = MaterialTheme.colorScheme.primary,
                            inactiveTrackColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f)
                        ),
                        modifier = Modifier.weight(1f)
                    )
                    Text(
                        text = "%.1fx".format(currentZoomRatio),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Save button
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center
            ) {
                Button(
                    onClick = {
                        val crop = croppedBitmap
                        val frame = fullFrame
                        if (crop != null && frame != null && saveState != SaveState.SAVING) {
                            saveState = SaveState.SAVING
                            saveDetectionPhotos(
                                context = context,
                                cropped = crop,
                                fullFrame = frame,
                                detection = detection,
                                classificationLabel = classificationLabel(classification),
                                onComplete = { success ->
                                    saveState = if (success) SaveState.SAVED else SaveState.IDLE
                                }
                            )
                        }
                    },
                    enabled = croppedBitmap != null && saveState != SaveState.SAVING,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (saveState == SaveState.SAVED) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.primary
                    )
                ) {
                    Icon(
                        imageVector = if (saveState == SaveState.SAVED) Icons.Default.CheckCircle else Icons.Default.Save,
                        contentDescription = "Save",
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = when (saveState) {
                            SaveState.IDLE -> "Save Photo"
                            SaveState.SAVING -> "Saving..."
                            SaveState.SAVED -> "Saved"
                        }
                    )
                }
            }

            if (autoSaved) {
                Text(
                    text = "Auto-saved to FriendOrFoe album",
                    color = MaterialTheme.colorScheme.secondary,
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp),
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Info section
            Text(
                text = "NOT in any aircraft database",
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                modifier = Modifier
                    .background(MaterialTheme.colorScheme.error.copy(alpha = 0.15f), RoundedCornerShape(4.dp))
                    .padding(horizontal = 8.dp, vertical = 4.dp)
            )

            Spacer(modifier = Modifier.height(4.dp))

            // Detection details
            val mlLabel = detection.labels.firstOrNull() ?: "Unknown"
            val conf = detection.labelConfidences.firstOrNull()?.let { "%.0f%%".format(it * 100) } ?: ""
            Text(
                text = "ML label: $mlLabel $conf",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall
            )
            Text(
                text = "Sky score: %.2f | Motion: %.2f".format(detection.skyScore, detection.motionScore),
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                style = MaterialTheme.typography.labelSmall
            )

            Spacer(modifier = Modifier.height(16.dp))
        }
    }
}

private enum class SaveState { IDLE, SAVING, SAVED }

/**
 * Save both cropped detection and full frame with bounding box to the device gallery.
 * Files go to Pictures/FriendOrFoe/ album.
 */
private fun saveDetectionPhotos(
    context: Context,
    cropped: Bitmap,
    fullFrame: Bitmap,
    detection: VisualDetection,
    classificationLabel: String,
    onComplete: ((Boolean) -> Unit)? = null
) {
    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    val safeLabel = classificationLabel.replace(" ", "_").lowercase()

    Thread {
        try {
            // 1. Save cropped detection
            saveBitmapToGallery(
                context = context,
                bitmap = cropped,
                fileName = "friendorfoe_${safeLabel}_crop_$timestamp",
                description = "FriendOrFoe detection: $classificationLabel"
            )

            // 2. Save full frame with bounding box overlay
            val annotated = fullFrame.copy(Bitmap.Config.ARGB_8888, true)
            val canvas = Canvas(annotated)
            val paint = Paint().apply {
                color = android.graphics.Color.RED
                style = Paint.Style.STROKE
                strokeWidth = 4f
            }
            val frameW = annotated.width.toFloat()
            val frameH = annotated.height.toFloat()
            val halfW = detection.width / 2f
            val halfH = detection.height / 2f
            val left = (detection.centerX - halfW) * frameW
            val top = (detection.centerY - halfH) * frameH
            val right = (detection.centerX + halfW) * frameW
            val bottom = (detection.centerY + halfH) * frameH
            canvas.drawRect(left, top, right, bottom, paint)

            // Draw label
            val textPaint = Paint().apply {
                color = android.graphics.Color.RED
                textSize = 32f
                isAntiAlias = true
            }
            canvas.drawText(classificationLabel, left, (top - 8f).coerceAtLeast(32f), textPaint)

            saveBitmapToGallery(
                context = context,
                bitmap = annotated,
                fileName = "friendorfoe_${safeLabel}_full_$timestamp",
                description = "FriendOrFoe full frame: $classificationLabel"
            )
            annotated.recycle()

            android.os.Handler(android.os.Looper.getMainLooper()).post {
                Toast.makeText(context, "Photo saved to FriendOrFoe album", Toast.LENGTH_SHORT).show()
                onComplete?.invoke(true)
            }
        } catch (e: Exception) {
            Log.e("ZoomViewSheet", "Failed to save photo", e)
            android.os.Handler(android.os.Looper.getMainLooper()).post {
                Toast.makeText(context, "Failed to save photo", Toast.LENGTH_SHORT).show()
                onComplete?.invoke(false)
            }
        }
    }.start()
}

/**
 * Save a bitmap to the device gallery via MediaStore (no permissions needed on API 29+).
 */
private fun saveBitmapToGallery(
    context: Context,
    bitmap: Bitmap,
    fileName: String,
    description: String
) {
    val contentValues = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, "$fileName.jpg")
        put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
        put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/FriendOrFoe")
        put(MediaStore.Images.Media.DESCRIPTION, description)
    }

    val resolver = context.contentResolver
    val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, contentValues)
        ?: throw IllegalStateException("Failed to create MediaStore entry")

    resolver.openOutputStream(uri)?.use { outputStream ->
        bitmap.compress(Bitmap.CompressFormat.JPEG, 95, outputStream)
    } ?: throw IllegalStateException("Failed to open output stream")
}

/**
 * Crop the detection region from the frame with generous padding.
 * Uses 50% padding around the detection and enforces a minimum crop size
 * of 25% of the frame to ensure the crop is large enough to be useful.
 */
internal fun cropDetection(frame: Bitmap, detection: VisualDetection): Bitmap? {
    return try {
        val padding = 0.5f
        val frameW = frame.width.toFloat()
        val frameH = frame.height.toFloat()

        // Use detection size with padding, but enforce minimum 25% of frame
        val rawHalfW = detection.width / 2f * (1f + padding)
        val rawHalfH = detection.height / 2f * (1f + padding)
        val halfW = maxOf(rawHalfW, 0.125f) // min 25% of frame width
        val halfH = maxOf(rawHalfH, 0.125f) // min 25% of frame height

        val left = ((detection.centerX - halfW) * frameW).toInt().coerceIn(0, frame.width - 1)
        val top = ((detection.centerY - halfH) * frameH).toInt().coerceIn(0, frame.height - 1)
        val right = ((detection.centerX + halfW) * frameW).toInt().coerceIn(left + 1, frame.width)
        val bottom = ((detection.centerY + halfH) * frameH).toInt().coerceIn(top + 1, frame.height)

        val cropW = right - left
        val cropH = bottom - top
        if (cropW < 2 || cropH < 2) return null

        Bitmap.createBitmap(frame, left, top, cropW, cropH)
    } catch (e: Exception) {
        null
    }
}

private fun classificationColor(classification: VisualClassification?): Color {
    return when (classification) {
        VisualClassification.LIKELY_DRONE -> Color(0xFFF44336)   // red
        VisualClassification.LIKELY_AIRCRAFT -> Color(0xFF4CAF50)  // green
        VisualClassification.LIKELY_BIRD -> Color(0xFF9E9E9E)    // gray
        VisualClassification.UNKNOWN_FLYING -> Color(0xFFFF9800)  // amber
        VisualClassification.LIKELY_STATIC -> Color(0xFF757575)   // dim gray
        null -> Color(0xFFFF9800)
    }
}

private fun classificationLabel(classification: VisualClassification?): String {
    return when (classification) {
        VisualClassification.LIKELY_DRONE -> "Possible Drone"
        VisualClassification.LIKELY_AIRCRAFT -> "Possible Aircraft"
        VisualClassification.LIKELY_BIRD -> "Possible Bird"
        VisualClassification.UNKNOWN_FLYING -> "Unknown Flying Object"
        VisualClassification.LIKELY_STATIC -> "Static Object"
        null -> "Unknown Object"
    }
}
