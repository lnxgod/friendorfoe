package com.friendorfoe.presentation.ar

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.Rect
import android.graphics.YuvImage
import java.io.ByteArrayOutputStream

internal fun normalizeJpegCapture(
    jpegBytes: ByteArray,
    sourceWidth: Int,
    sourceHeight: Int,
    rotationDegrees: Int,
): CapturePayload {
    val rotation = ((rotationDegrees % 360) + 360) % 360
    require(rotation in setOf(0, 90, 180, 270)) {
        "Unsupported camera rotation: $rotationDegrees"
    }
    if (rotation == 0) {
        return CapturePayload(jpegBytes, "image/jpeg", sourceWidth, sourceHeight)
    }

    val source = checkNotNull(BitmapFactory.decodeByteArray(jpegBytes, 0, jpegBytes.size)) {
        "Could not decode captured JPEG"
    }
    val rotated = Bitmap.createBitmap(
        source,
        0,
        0,
        source.width,
        source.height,
        Matrix().apply { postRotate(rotation.toFloat()) },
        true,
    )
    return try {
        val uprightBytes = ByteArrayOutputStream().use { output ->
            check(rotated.compress(Bitmap.CompressFormat.JPEG, 95, output)) {
                "Could not encode upright camera frame"
            }
            output.toByteArray()
        }
        CapturePayload(
            bytes = uprightBytes,
            mimeType = "image/jpeg",
            widthPx = rotated.width,
            heightPx = rotated.height,
        )
    } finally {
        if (rotated !== source) rotated.recycle()
        source.recycle()
    }
}

internal fun capturePayloadFromNv21(
    nv21: ByteArray,
    sourceWidth: Int,
    sourceHeight: Int,
    rotationDegrees: Int,
): CapturePayload {
    val jpegBytes = ByteArrayOutputStream().use { output ->
        val compressed = YuvImage(
            nv21,
            ImageFormat.NV21,
            sourceWidth,
            sourceHeight,
            null,
        ).compressToJpeg(Rect(0, 0, sourceWidth, sourceHeight), 95, output)
        check(compressed) { "Could not encode camera frame" }
        output.toByteArray()
    }
    return normalizeJpegCapture(
        jpegBytes = jpegBytes,
        sourceWidth = sourceWidth,
        sourceHeight = sourceHeight,
        rotationDegrees = rotationDegrees,
    )
}
