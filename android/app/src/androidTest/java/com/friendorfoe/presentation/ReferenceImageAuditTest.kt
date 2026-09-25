package com.friendorfoe.presentation

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.R
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.components.ReferenceImage
import com.friendorfoe.presentation.detail.AircraftPhotoCard
import com.friendorfoe.presentation.detail.AircraftVisual
import com.friendorfoe.presentation.drones.DroneReferenceScreen
import com.friendorfoe.presentation.aircraft.AircraftReferenceScreen
import com.friendorfoe.presentation.map.createCategoryMarkerDrawable
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.util.*
import java.io.File
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class ReferenceImageAuditTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun everyBundledJpegDecodesOnAndroid() {
        val failures = mutableListOf<String>()
        for (folder in listOf("aircraft", "drones")) {
            val photos = context.assets.list(folder).orEmpty().filter { it.endsWith(".jpg") }
            assertTrue("Empty $folder catalog", photos.isNotEmpty())
            photos.forEach { name ->
                val path = "$folder/$name"
                val bitmap = context.assets.open(path).use { BitmapFactory.decodeStream(it) }
                if (bitmap == null || bitmap.width < 120 || bitmap.height < 120) failures += path
                bitmap?.recycle()
            }
        }
        assertEquals(emptyList<String>(), failures)
    }

    @Test fun everySilhouetteAndMapMarkerRendersWithoutClippedEdges() {
        SilhouetteCategory.entries.forEach { category ->
            val drawable = checkNotNull(ContextCompat.getDrawable(context, silhouetteDrawableRes(category)))
            val bitmap = Bitmap.createBitmap(120, 120, Bitmap.Config.ARGB_8888)
            drawable.setBounds(0, 0, 120, 120)
            drawable.draw(Canvas(bitmap))
            val pixels = IntArray(120 * 120)
            bitmap.getPixels(pixels, 0, 120, 0, 0, 120, 120)
            assertTrue("Blank $category", pixels.any { it ushr 24 != 0 })
            bitmap.recycle()
        }
        ObjectCategory.entries.forEach { category ->
            for (heading in listOf(0f, 45f, 90f)) {
                val marker = createCategoryMarkerDrawable(context, category, android.graphics.Color.CYAN, heading)
                val size = marker.intrinsicWidth
                val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
                marker.setBounds(0, 0, size, size)
                marker.draw(Canvas(bitmap))
                for (i in 0 until size) {
                    assertEquals("Clipped $category at $heading", 0, bitmap.getPixel(i, 0) ushr 24)
                    assertEquals("Clipped $category at $heading", 0, bitmap.getPixel(i, size - 1) ushr 24)
                    assertEquals("Clipped $category at $heading", 0, bitmap.getPixel(0, i) ushr 24)
                    assertEquals("Clipped $category at $heading", 0, bitmap.getPixel(size - 1, i) ushr 24)
                }
                bitmap.recycle()
            }
        }
    }

    @Test fun failedPhotoFallsBackToBundledTypePhoto() {
        val corrupt = File(context.cacheDir, "image-audit-corrupt.jpg").apply { writeText("not an image") }
        try {
            compose.setContent { FriendOrFoeTheme { Surface {
                AircraftPhotoCard(AircraftVisual(corrupt.toURI().toString(), "B738", "Boeing 737", ObjectCategory.COMMERCIAL))
            } } }
            waitForImage("detail_aircraft_photo_image")
            compose.onNodeWithText("Type reference").assertIsDisplayed()
            capture("detail-photo-fallback.png")
        } finally { corrupt.delete() }
    }

    @Test fun missingReferencePhotoHasAnIntentionalVisibleFallback() {
        compose.setContent { FriendOrFoeTheme { Surface {
            ReferenceImage("file:///android_asset/missing.jpg", "Drone", R.drawable.ic_silhouette_drone,
                Modifier.width(360.dp).height(200.dp))
        } } }
        compose.waitUntil(5_000) { compose.onAllNodesWithText("Photo unavailable").fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithContentDescription("Drone · category illustration").assertIsDisplayed()
        capture("reference-fallback-dark.png")
    }

    @Test fun rejectedPhotoShowsTheFallbackInTheRealReferenceScreen() {
        compose.setContent { FriendOrFoeTheme { Surface { DroneReferenceScreen({}, "Hubsan") } } }
        compose.onNodeWithText("Photo unavailable").assertIsDisplayed()
        capture("reference-withheld-dark.png")
    }

    @Test fun correctedDronePhotosRenderInTheReferenceGuide() {
        compose.setContent { FriendOrFoeTheme { Surface { DroneReferenceScreen({}, "DJI") } } }
        waitForImage("reference_photo_image")
        capture("drone-reference-dark.png")
    }

    @Test fun correctedAircraftPhotoRendersInLightMode() {
        compose.setContent { FriendOrFoeTheme(darkTheme = false) { Surface { AircraftReferenceScreen({}, "FA18") } } }
        waitForImage("reference_photo_image")
        capture("aircraft-reference-light.png")
    }

    private fun waitForImage(tag: String) {
        compose.waitUntil(10_000) { compose.onAllNodesWithTag(tag, useUnmergedTree = true).fetchSemanticsNodes().isNotEmpty() }
    }

    private fun capture(name: String) {
        compose.mainClock.advanceTimeBy(500)
        compose.waitForIdle()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(context.filesDir, "image-audit-$name").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
    }
}
