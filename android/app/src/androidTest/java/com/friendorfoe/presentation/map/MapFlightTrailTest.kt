package com.friendorfoe.presentation.map

import android.graphics.Bitmap
import android.graphics.Canvas
import android.view.MotionEvent
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Overlay
import org.osmdroid.views.overlay.Polyline

class MapFlightTrailTest {
    @get:Rule val compose = createComposeRule()
    private fun point(time: Long, lat: Double) = TrackingEntity(objectId = "demo", latitude = lat,
        longitude = -117.1, altitudeMeters = 1000.0, heading = null, speedMps = null, timestamp = time)

    @Test fun overlayPreservesOtherLayersSplitsGapsAndOpensRecordedEndpoint() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        instrumentation.runOnMainSync {
            val map = MapView(instrumentation.targetContext).apply {
                layout(0, 0, 480, 800)
                controller.setZoom(14.0)
                controller.setCenter(GeoPoint(32.702, -117.1))
            }
            val unrelated = object : Overlay() {}
            map.overlays.add(unrelated)
            var opened: String? = null
            val layer = MapFlightTrailOverlay(map) { opened = it }
            val trail = MapFlightTrail("demo", "DEMO", listOf(point(1000, 32.7), point(11_000, 32.701),
                point(611_000, 32.702), point(621_000, 32.703)), false)
            layer.render(listOf(trail), null)
            layer.render(listOf(trail), null)
            assertEquals(2, map.overlays.filterIsInstance<Polyline>().size)
            assertEquals(1, map.overlays.filterIsInstance<Marker>().size)
            val bitmap = Bitmap.createBitmap(480, 800, Bitmap.Config.ARGB_8888)
            val marker = map.overlays.filterIsInstance<Marker>().single()
            marker.draw(Canvas(bitmap), map, false)
            val pixel = map.projection.toPixels(marker.position, null)
            val event = MotionEvent.obtain(0, 1, MotionEvent.ACTION_UP, pixel.x.toFloat(), pixel.y.toFloat(), 0)
            assertTrue(marker.onSingleTapConfirmed(event, map))
            assertEquals("demo", opened)
            layer.render(emptyList(), null)
            assertEquals(listOf(unrelated), map.overlays.toList())
            event.recycle()
            bitmap.recycle()
            map.onDetach()
        }
    }

    @Test fun trailControlsOfferWindowsRetryAndDisableEmptyFit() {
        var chosen: FlightTrailWindow? = null
        var retried = false
        compose.setContent { FriendOrFoeTheme {
            MapFlightTrailControls(FlightTrailWindow.FIFTEEN_MINUTES, MapFlightTrailsState(error = true),
                { chosen = it }, {}, { retried = true })
        } }
        compose.onNodeWithTag("fit_flight_trails").assertIsNotEnabled()
        compose.onNodeWithTag("flight_trails_DAY").performClick()
        compose.runOnIdle { assertEquals(FlightTrailWindow.DAY, chosen) }
        compose.onNodeWithText("Couldn't load trails · Retry").performClick()
        compose.runOnIdle { assertTrue(retried) }
        compose.onNodeWithTag("flight_trails_OFF").performClick()
        compose.runOnIdle { assertEquals(FlightTrailWindow.OFF, chosen) }
    }
}
