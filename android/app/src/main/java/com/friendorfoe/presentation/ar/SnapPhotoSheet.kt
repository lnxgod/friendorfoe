package com.friendorfoe.presentation.ar

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import android.widget.Toast
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

/**
 * Data class representing a snap-to photo capture target.
 *
 * @param objectId Unique ID of the sky object (ICAO hex or drone ID)
 * @param label Display label (callsign, drone ID, etc.)
 * @param typeDescription Aircraft type or drone model description
 * @param distanceMeters Distance from user to the object, if known
 */
data class SnapTarget(
    val objectId: String,
    val label: String,
    val typeDescription: String?,
    val distanceMeters: Double?
)

/**
 * Bottom sheet for snap-to photo capture of identified aircraft/drones.
 *
 * Shows a live preview from the camera, a zoom slider spanning all forward
 * lenses (ultrawide to telephoto), preset zoom buttons, and a capture button
 * for full-resolution ImageCapture photos.
 *
 * @param target The snap target with object info
 * @param getFrame Callback to get the latest camera frame bitmap
 * @param currentZoomRatio Current camera zoom level
 * @param minZoomRatio Minimum zoom ratio (ultrawide, often 0.5x)
 * @param maxZoomRatio Maximum zoom ratio (telephoto limit)
 * @param onZoomChange Callback to change camera zoom
 * @param onCapture Callback to capture a full-resolution photo via ImageCapture
 * @param onViewDetails Callback to navigate to the detail screen for this object
 * @param onDismiss Callback when the sheet is dismissed
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SnapPhotoSheet(
    target: SnapTarget,
    getFrame: () -> Bitmap?,
    currentZoomRatio: Float,
    minZoomRatio: Float,
    maxZoomRatio: Float,
    onZoomChange: (Float) -> Unit,
    onCapture: (onResult: (Boolean) -> Unit) -> Unit,
    onViewDetails: () -> Unit,
    onDismiss: () -> Unit
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    // Live preview frame, auto-refreshed
    var previewFrame by remember { mutableStateOf<Bitmap?>(null) }

    // Capture state
    var captureState by remember { mutableStateOf(CaptureState.READY) }

    // Auto-refresh preview every 300ms
    LaunchedEffect(target.objectId) {
        while (true) {
            val frame = getFrame()
            if (frame != null) {
                previewFrame = frame
            }
            delay(300L)
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
            // Header: object label + type + distance
            Text(
                text = target.label,
                color = Color(0xFF00BCD4),
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = target.typeDescription ?: "Unknown Type",
                    color = Color.White.copy(alpha = 0.7f),
                    fontSize = 14.sp
                )
                target.distanceMeters?.let { dist ->
                    val distStr = if (dist > 800.0) {
                        "%.1f mi".format(dist / 1609.344)
                    } else {
                        "${dist.toInt()}m"
                    }
                    Text(
                        text = distStr,
                        color = Color.White.copy(alpha = 0.7f),
                        fontSize = 14.sp
                    )
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Live preview
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 200.dp, max = 300.dp)
                    .background(Color.Black, RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center
            ) {
                val bitmap = previewFrame
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap.asImageBitmap(),
                        contentDescription = "Live preview of ${target.label}",
                        contentScale = ContentScale.Fit,
                        modifier = Modifier.fillMaxWidth()
                    )

                    // Current zoom indicator
                    Text(
                        text = "%.1fx".format(currentZoomRatio),
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
                        text = "Focusing...",
                        color = Color.White.copy(alpha = 0.5f),
                        fontSize = 14.sp
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Zoom slider (ultrawide to telephoto)
            if (maxZoomRatio > minZoomRatio + 0.1f) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        text = "%.1fx".format(minZoomRatio),
                        color = Color.White.copy(alpha = 0.5f),
                        fontSize = 11.sp
                    )
                    Slider(
                        value = currentZoomRatio,
                        onValueChange = { onZoomChange(it) },
                        valueRange = minZoomRatio..maxZoomRatio,
                        colors = SliderDefaults.colors(
                            thumbColor = Color(0xFF00BCD4),
                            activeTrackColor = Color(0xFF00BCD4),
                            inactiveTrackColor = Color.White.copy(alpha = 0.3f)
                        ),
                        modifier = Modifier.weight(1f)
                    )
                    Text(
                        text = "%.0fx".format(maxZoomRatio),
                        color = Color.White.copy(alpha = 0.5f),
                        fontSize = 11.sp
                    )
                }

                // Zoom preset buttons
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceEvenly
                ) {
                    // Only show 0.5x if ultrawide is available (minZoom < 1.0)
                    if (minZoomRatio < 0.9f) {
                        ZoomPresetChip("0.5x", 0.5f, currentZoomRatio, minZoomRatio, maxZoomRatio, onZoomChange)
                    }
                    ZoomPresetChip("1x", 1.0f, currentZoomRatio, minZoomRatio, maxZoomRatio, onZoomChange)
                    ZoomPresetChip("2x", 2.0f, currentZoomRatio, minZoomRatio, maxZoomRatio, onZoomChange)
                    ZoomPresetChip("5x", 5.0f, currentZoomRatio, minZoomRatio, maxZoomRatio, onZoomChange)
                    ZoomPresetChip("Max", maxZoomRatio, currentZoomRatio, minZoomRatio, maxZoomRatio, onZoomChange)
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Capture button (large, centered)
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center
            ) {
                Button(
                    onClick = {
                        if (captureState == CaptureState.READY) {
                            captureState = CaptureState.CAPTURING
                            onCapture { success ->
                                captureState = if (success) CaptureState.SAVED else CaptureState.READY
                            }
                        }
                    },
                    modifier = Modifier
                        .size(72.dp),
                    shape = CircleShape,
                    enabled = captureState != CaptureState.CAPTURING,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = when (captureState) {
                            CaptureState.READY -> Color.White
                            CaptureState.CAPTURING -> Color.Gray
                            CaptureState.SAVED -> Color(0xFF4CAF50)
                        }
                    )
                ) {
                    Icon(
                        imageVector = when (captureState) {
                            CaptureState.SAVED -> Icons.Default.CheckCircle
                            else -> Icons.Default.CameraAlt
                        },
                        contentDescription = "Capture Photo",
                        tint = when (captureState) {
                            CaptureState.READY -> Color.Black
                            CaptureState.CAPTURING -> Color.White
                            CaptureState.SAVED -> Color.White
                        },
                        modifier = Modifier.size(32.dp)
                    )
                }
            }

            // Save confirmation text
            if (captureState == CaptureState.SAVED) {
                Text(
                    text = "Saved to Pictures/FriendOrFoe",
                    color = Color(0xFF4CAF50).copy(alpha = 0.8f),
                    fontSize = 12.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp),
                    textAlign = TextAlign.Center
                )

            }

            // Reset capture state after a moment so user can capture again
            LaunchedEffect(captureState) {
                if (captureState == CaptureState.SAVED) {
                    delay(2000L)
                    captureState = CaptureState.READY
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // View Details button
            TextButton(
                onClick = onViewDetails,
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(
                    imageVector = Icons.Default.Info,
                    contentDescription = "View Details",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text("View Details")
            }

            Spacer(modifier = Modifier.height(8.dp))
        }
    }
}

@Composable
private fun ZoomPresetChip(
    label: String,
    targetZoom: Float,
    currentZoom: Float,
    minZoom: Float,
    maxZoom: Float,
    onZoomChange: (Float) -> Unit
) {
    val clamped = targetZoom.coerceIn(minZoom, maxZoom)
    val isSelected = kotlin.math.abs(currentZoom - clamped) < 0.15f

    FilterChip(
        selected = isSelected,
        onClick = { onZoomChange(clamped) },
        label = {
            Text(
                text = label,
                fontSize = 12.sp
            )
        },
        colors = FilterChipDefaults.filterChipColors(
            selectedContainerColor = Color(0xFF00BCD4),
            selectedLabelColor = Color.White
        )
    )
}

private enum class CaptureState { READY, CAPTURING, SAVED }

/**
 * Capture a full-resolution photo using ImageCapture and save to MediaStore.
 * Files go to Pictures/FriendOrFoe/ album.
 */
fun capturePhotoToGallery(
    context: Context,
    imageCapture: ImageCapture,
    label: String,
    onComplete: (Boolean) -> Unit
) {
    val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
        .format(java.util.Date())
    val safeLabel = label.replace(Regex("[^a-zA-Z0-9]"), "_").lowercase()

    val contentValues = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, "friendorfoe_${safeLabel}_$timestamp.jpg")
        put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
        put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/FriendOrFoe")
        put(MediaStore.Images.Media.DESCRIPTION, "FriendOrFoe capture: $label")
    }

    val outputOptions = ImageCapture.OutputFileOptions.Builder(
        context.contentResolver,
        MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
        contentValues
    ).build()

    imageCapture.takePicture(
        outputOptions,
        androidx.core.content.ContextCompat.getMainExecutor(context),
        object : ImageCapture.OnImageSavedCallback {
            override fun onImageSaved(output: ImageCapture.OutputFileResults) {
                Log.d("SnapPhotoSheet", "Photo saved: ${output.savedUri}")
                Toast.makeText(context, "Photo saved to FriendOrFoe album", Toast.LENGTH_SHORT).show()
                onComplete(true)
            }

            override fun onError(exception: ImageCaptureException) {
                Log.e("SnapPhotoSheet", "Photo capture failed", exception)
                Toast.makeText(context, "Photo capture failed", Toast.LENGTH_SHORT).show()
                onComplete(false)
            }
        }
    )
}
