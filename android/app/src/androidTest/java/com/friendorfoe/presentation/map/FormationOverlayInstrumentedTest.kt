package com.friendorfoe.presentation.map

import android.graphics.Bitmap
import android.graphics.Canvas
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView

@RunWith(AndroidJUnit4::class)
class FormationOverlayInstrumentedTest {

    @Test
    fun repeatedlyDrawsMaximumBoardAsOneOverlay() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        instrumentation.runOnMainSync {
            val context = instrumentation.targetContext
            val mapView = MapView(context).apply {
                layout(0, 0, 480, 800)
                controller.setZoom(18.0)
                controller.setCenter(GeoPoint(37.0, -122.0))
            }
            val overlay = FormationOverlay(context)
            val points = List(240) { index ->
                val row = index / 20
                val column = index % 20
                GeoPoint(
                    37.0 + row * 0.00001,
                    -122.0 + column * 0.00001,
                )
            }
            val bitmap = Bitmap.createBitmap(480, 800, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)

            repeat(500) {
                overlay.replacePoints(points)
                overlay.draw(canvas, mapView, false)
            }

            assertEquals(240, overlay.pointCount)
            mapView.onDetach()
            bitmap.recycle()
        }
    }
}
