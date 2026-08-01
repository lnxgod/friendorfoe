package com.friendorfoe.presentation.privacy

typealias FloatPoint = com.friendorfoe.detection.FloatPoint

data class IntRect(
    val left: Int,
    val top: Int,
    val right: Int,
    val bottom: Int,
) {
    val width: Int get() = right - left
    val height: Int get() = bottom - top
}

data class AnalysisFrameMetadata(
    val imageWidth: Int,
    val imageHeight: Int,
    val crop: IntRect,
    val rotationDegrees: Int,
    val frontCamera: Boolean,
)

fun transformAnalysisPoint(
    source: FloatPoint,
    metadata: AnalysisFrameMetadata,
    previewWidth: Float,
    previewHeight: Float,
): FloatPoint {
    require(metadata.imageWidth > 0 && metadata.imageHeight > 0) {
        "Analysis dimensions must be positive"
    }
    require(metadata.crop.width > 0 && metadata.crop.height > 0) {
        "Analysis crop must be non-empty"
    }
    require(metadata.rotationDegrees in setOf(0, 90, 180, 270)) {
        "Rotation must be 0, 90, 180, or 270"
    }
    require(previewWidth > 0f && previewHeight > 0f) {
        "Preview dimensions must be positive"
    }

    val u = ((source.x - metadata.crop.left) / metadata.crop.width).coerceIn(0f, 1f)
    val v = ((source.y - metadata.crop.top) / metadata.crop.height).coerceIn(0f, 1f)
    val rotated = when (metadata.rotationDegrees) {
        0 -> FloatPoint(u, v)
        90 -> FloatPoint(1f - v, u)
        180 -> FloatPoint(1f - u, 1f - v)
        270 -> FloatPoint(v, 1f - u)
        else -> error("Rotation validation failed")
    }
    val mirrored = if (metadata.frontCamera) {
        FloatPoint(1f - rotated.x, rotated.y)
    } else {
        rotated
    }
    val rotatedWidth = if (metadata.rotationDegrees % 180 == 0) {
        metadata.crop.width.toFloat()
    } else {
        metadata.crop.height.toFloat()
    }
    val rotatedHeight = if (metadata.rotationDegrees % 180 == 0) {
        metadata.crop.height.toFloat()
    } else {
        metadata.crop.width.toFloat()
    }
    val scale = maxOf(previewWidth / rotatedWidth, previewHeight / rotatedHeight)
    val xCrop = (rotatedWidth * scale - previewWidth) / 2f
    val yCrop = (rotatedHeight * scale - previewHeight) / 2f
    return FloatPoint(
        x = mirrored.x * rotatedWidth * scale - xCrop,
        y = mirrored.y * rotatedHeight * scale - yCrop,
    )
}
