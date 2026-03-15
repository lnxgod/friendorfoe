package com.friendorfoe.presentation.map

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.util.categoryColorArgb

/**
 * Creates a distinct map marker drawable for each ObjectCategory.
 * Each category gets a unique silhouette shape, rotated by heading.
 */
fun createCategoryMarkerDrawable(
    context: Context,
    category: ObjectCategory,
    color: Int,
    heading: Float
): Drawable {
    val density = context.resources.displayMetrics.density
    val config = MARKER_CONFIG[category] ?: MarkerConfig(18, 1.5f)
    val sizePx = (config.sizeDp * density).toInt()
    val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)

    canvas.save()
    canvas.rotate(heading, sizePx / 2f, sizePx / 2f)

    val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color
        style = Paint.Style.FILL
    }
    val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = 0xFF000000.toInt()
        style = Paint.Style.STROKE
        strokeWidth = config.borderWidth * density
    }

    val cx = sizePx / 2f
    val cy = sizePx / 2f
    val u = sizePx / 10f

    val path = when (category) {
        ObjectCategory.COMMERCIAL -> airlinerPath(cx, cy, u)
        ObjectCategory.GENERAL_AVIATION -> cessnaPath(cx, cy, u)
        ObjectCategory.MILITARY -> fighterPath(cx, cy, u)
        ObjectCategory.HELICOPTER -> helicopterPath(cx, cy, u)
        ObjectCategory.GOVERNMENT -> airlinerPath(cx, cy, u)
        ObjectCategory.EMERGENCY -> airlinerPath(cx, cy, u)
        ObjectCategory.CARGO -> cargoPath(cx, cy, u)
        ObjectCategory.DRONE -> quadcopterPath(cx, cy, u)
        ObjectCategory.GROUND_VEHICLE -> vehiclePath(cx, cy, u)
        ObjectCategory.UNKNOWN -> diamondPath(cx, cy, u)
    }

    canvas.drawPath(path, fillPaint)
    canvas.drawPath(path, borderPaint)

    // Helicopter gets a rotor circle on top
    if (category == ObjectCategory.HELICOPTER) {
        val rotorPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.color = color
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * density
        }
        canvas.drawCircle(cx, cy - 1.5f * u, 3.5f * u, rotorPaint)
    }

    canvas.restore()
    return BitmapDrawable(context.resources, bitmap)
}

// Airliner: wider wings with engine bumps
private fun airlinerPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 4.5f * u)              // nose
    lineTo(cx + 0.8f * u, cy - 3f * u)     // right fuselage
    lineTo(cx + 4.5f * u, cy + 0.5f * u)   // right wingtip
    lineTo(cx + 4.5f * u, cy + 1.2f * u)   // wing trailing edge
    lineTo(cx + 1f * u, cy + 1f * u)       // right body
    // Right engine bump
    lineTo(cx + 2.5f * u, cy + 0f * u)
    lineTo(cx + 2.5f * u, cy + 1f * u)
    lineTo(cx + 1f * u, cy + 1f * u)
    // Tail
    lineTo(cx + 1f * u, cy + 3.5f * u)
    lineTo(cx + 2.2f * u, cy + 4f * u)     // right tail
    lineTo(cx + 2.2f * u, cy + 4.5f * u)
    lineTo(cx, cy + 3.8f * u)              // tail center
    lineTo(cx - 2.2f * u, cy + 4.5f * u)
    lineTo(cx - 2.2f * u, cy + 4f * u)     // left tail
    lineTo(cx - 1f * u, cy + 3.5f * u)
    // Left engine bump
    lineTo(cx - 1f * u, cy + 1f * u)
    lineTo(cx - 2.5f * u, cy + 1f * u)
    lineTo(cx - 2.5f * u, cy + 0f * u)
    lineTo(cx - 1f * u, cy + 1f * u)
    // Left wing
    lineTo(cx - 4.5f * u, cy + 1.2f * u)
    lineTo(cx - 4.5f * u, cy + 0.5f * u)
    lineTo(cx - 0.8f * u, cy - 3f * u)     // left fuselage
    close()
}

// Cessna: high wing, straight, smaller proportions
private fun cessnaPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 4f * u)                // nose
    lineTo(cx + 0.6f * u, cy - 2.5f * u)
    // High straight wing
    lineTo(cx + 4f * u, cy - 1.5f * u)
    lineTo(cx + 4f * u, cy - 0.8f * u)
    lineTo(cx + 0.6f * u, cy - 0.5f * u)
    // Body to tail
    lineTo(cx + 0.6f * u, cy + 3f * u)
    lineTo(cx + 2f * u, cy + 3.8f * u)     // right tail
    lineTo(cx + 2f * u, cy + 4.3f * u)
    lineTo(cx, cy + 3.5f * u)
    lineTo(cx - 2f * u, cy + 4.3f * u)
    lineTo(cx - 2f * u, cy + 3.8f * u)     // left tail
    lineTo(cx - 0.6f * u, cy + 3f * u)
    // Left wing
    lineTo(cx - 0.6f * u, cy - 0.5f * u)
    lineTo(cx - 4f * u, cy - 0.8f * u)
    lineTo(cx - 4f * u, cy - 1.5f * u)
    lineTo(cx - 0.6f * u, cy - 2.5f * u)
    close()
}

// Fighter: delta/swept wings, angular nose
private fun fighterPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 4.8f * u)              // sharp nose
    lineTo(cx + 0.5f * u, cy - 3f * u)
    // Swept right wing
    lineTo(cx + 4.5f * u, cy + 2f * u)
    lineTo(cx + 3.5f * u, cy + 2.5f * u)
    lineTo(cx + 1f * u, cy + 1f * u)
    // Right tail
    lineTo(cx + 1f * u, cy + 3.5f * u)
    lineTo(cx + 2.5f * u, cy + 4.5f * u)
    lineTo(cx + 2f * u, cy + 4.8f * u)
    lineTo(cx, cy + 4f * u)
    // Left tail
    lineTo(cx - 2f * u, cy + 4.8f * u)
    lineTo(cx - 2.5f * u, cy + 4.5f * u)
    lineTo(cx - 1f * u, cy + 3.5f * u)
    // Swept left wing
    lineTo(cx - 1f * u, cy + 1f * u)
    lineTo(cx - 3.5f * u, cy + 2.5f * u)
    lineTo(cx - 4.5f * u, cy + 2f * u)
    lineTo(cx - 0.5f * u, cy - 3f * u)
    close()
}

// Helicopter: teardrop body (rotor circle drawn separately)
private fun helicopterPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 2f * u)                // nose
    lineTo(cx + 1.5f * u, cy)
    lineTo(cx + 1.2f * u, cy + 2f * u)
    // Tail boom
    lineTo(cx + 0.4f * u, cy + 2f * u)
    lineTo(cx + 0.3f * u, cy + 4.5f * u)
    // Tail rotor
    lineTo(cx + 1.5f * u, cy + 4.2f * u)
    lineTo(cx + 1.5f * u, cy + 4.8f * u)
    lineTo(cx - 1.5f * u, cy + 4.8f * u)
    lineTo(cx - 1.5f * u, cy + 4.2f * u)
    lineTo(cx - 0.3f * u, cy + 4.5f * u)
    lineTo(cx - 0.4f * u, cy + 2f * u)
    lineTo(cx - 1.2f * u, cy + 2f * u)
    lineTo(cx - 1.5f * u, cy)
    close()
}

// Cargo: high wing, wide body, upswept tail
private fun cargoPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 4f * u)                // nose
    lineTo(cx + 1.2f * u, cy - 2.5f * u)
    // High right wing
    lineTo(cx + 4.5f * u, cy - 2f * u)
    lineTo(cx + 4.5f * u, cy - 1.2f * u)
    lineTo(cx + 1.2f * u, cy - 0.5f * u)
    // Wide body
    lineTo(cx + 1.2f * u, cy + 2.5f * u)
    // Upswept tail
    lineTo(cx + 2.5f * u, cy + 3f * u)
    lineTo(cx + 2.5f * u, cy + 3.8f * u)
    lineTo(cx + 0.5f * u, cy + 4.5f * u)
    lineTo(cx, cy + 4.2f * u)
    lineTo(cx - 0.5f * u, cy + 4.5f * u)
    lineTo(cx - 2.5f * u, cy + 3.8f * u)
    lineTo(cx - 2.5f * u, cy + 3f * u)
    lineTo(cx - 1.2f * u, cy + 2.5f * u)
    // Left wing
    lineTo(cx - 1.2f * u, cy - 0.5f * u)
    lineTo(cx - 4.5f * u, cy - 1.2f * u)
    lineTo(cx - 4.5f * u, cy - 2f * u)
    lineTo(cx - 1.2f * u, cy - 2.5f * u)
    close()
}

// Quadcopter: 4-arm star with small circles
private fun quadcopterPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    val armLen = 3f * u
    val armW = 0.5f * u
    // Center body
    addCircle(cx, cy, 1.2f * u, Path.Direction.CW)
    // Four arms at 45/135/225/315 degrees
    for (angle in listOf(45f, 135f, 225f, 315f)) {
        val rad = Math.toRadians(angle.toDouble())
        val cos = kotlin.math.cos(rad).toFloat()
        val sin = kotlin.math.sin(rad).toFloat()
        val perpCos = kotlin.math.cos(rad + Math.PI / 2).toFloat()
        val perpSin = kotlin.math.sin(rad + Math.PI / 2).toFloat()
        // Arm rectangle
        moveTo(cx + perpCos * armW, cy + perpSin * armW)
        lineTo(cx + cos * armLen + perpCos * armW, cy + sin * armLen + perpSin * armW)
        lineTo(cx + cos * armLen - perpCos * armW, cy + sin * armLen - perpSin * armW)
        lineTo(cx - perpCos * armW, cy - perpSin * armW)
        close()
        // Rotor circle at end of arm
        addCircle(cx + cos * armLen, cy + sin * armLen, 1.2f * u, Path.Direction.CW)
    }
}

// Ground vehicle: rounded rectangle
private fun vehiclePath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    val w = 2.5f * u
    val h = 3.5f * u
    val r = 0.8f * u
    addRoundRect(cx - w, cy - h, cx + w, cy + h, r, r, Path.Direction.CW)
}

// Unknown: diamond
private fun diamondPath(cx: Float, cy: Float, u: Float): Path = Path().apply {
    moveTo(cx, cy - 3.5f * u)    // top
    lineTo(cx + 3f * u, cy)       // right
    lineTo(cx, cy + 3.5f * u)     // bottom
    lineTo(cx - 3f * u, cy)       // left
    close()
}

private data class MarkerConfig(val sizeDp: Int, val borderWidth: Float)

private val MARKER_CONFIG = mapOf(
    ObjectCategory.COMMERCIAL to MarkerConfig(22, 1.5f),
    ObjectCategory.GENERAL_AVIATION to MarkerConfig(18, 1.5f),
    ObjectCategory.MILITARY to MarkerConfig(26, 3f),
    ObjectCategory.HELICOPTER to MarkerConfig(22, 1.5f),
    ObjectCategory.GOVERNMENT to MarkerConfig(24, 2.5f),
    ObjectCategory.EMERGENCY to MarkerConfig(24, 3f),
    ObjectCategory.CARGO to MarkerConfig(26, 2f),
    ObjectCategory.DRONE to MarkerConfig(18, 1.5f),
    ObjectCategory.GROUND_VEHICLE to MarkerConfig(16, 1.5f),
    ObjectCategory.UNKNOWN to MarkerConfig(16, 1f)
)
