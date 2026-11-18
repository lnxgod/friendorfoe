package com.friendorfoe.presentation.map

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.graphics.luminance
import org.osmdroid.views.MapView

@Composable
internal fun ApplyMapAppearance(mapView: MapView) {
    val isDarkTheme = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    // Neutral luminance keeps labels readable without inverting parks and water into neon hues.
    LaunchedEffect(mapView, isDarkTheme) {
        if (isDarkTheme) {
            mapView.overlayManager.tilesOverlay.setColorFilter(
                android.graphics.ColorMatrixColorFilter(
                    android.graphics.ColorMatrix(floatArrayOf(
                        -0.128f, -0.429f, -0.043f, 0f, 170f,
                        -0.128f, -0.429f, -0.043f, 0f, 179f,
                        -0.128f, -0.429f, -0.043f, 0f, 181f,
                         0f,      0f,      0f,     1f,   0f
                    ))
                )
            )
        } else {
            mapView.overlayManager.tilesOverlay.setColorFilter(null)
        }
        mapView.invalidate()
    }
}
