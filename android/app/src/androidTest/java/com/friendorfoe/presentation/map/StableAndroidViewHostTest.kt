package com.friendorfoe.presentation.map

import android.content.Context
import android.view.MotionEvent
import android.view.View
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.click
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.junit4.StateRestorationTester
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class StableAndroidViewHostTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun temporarilyHiddenMapViewRemainsAttachedAndIsNotRecreated() {
        val revealed = mutableStateOf(true)
        var factoryCalls = 0
        var detachCalls = 0
        lateinit var originalView: View
        lateinit var updatedView: View

        compose.setContent {
            StableAndroidViewHost(
                revealed = revealed.value,
                factory = { context ->
                    TrackingView(context) { detachCalls++ }.also {
                        factoryCalls++
                        originalView = it
                    }
                },
                update = { updatedView = it },
            )
        }
        compose.waitForIdle()

        compose.runOnUiThread { revealed.value = false }
        compose.waitForIdle()
        compose.runOnUiThread { revealed.value = true }
        compose.waitForIdle()

        assertEquals(1, factoryCalls)
        assertEquals(0, detachCalls)
        assertSame(originalView, updatedView)
    }

    @Test
    fun cameraOwnershipDoesNotRestoreOntoARecreatedNativeMap() {
        val restoration = StateRestorationTester(compose)
        lateinit var currentMap: Any
        lateinit var originalMap: Any
        lateinit var ownership: androidx.compose.runtime.MutableState<Boolean>

        restoration.setContent {
            currentMap = androidx.compose.runtime.remember { Any() }
            ownership = rememberMapCameraOwnership(currentMap)
        }
        compose.runOnIdle {
            originalMap = currentMap
            ownership.value = true
        }

        restoration.emulateSavedInstanceStateRestore()

        compose.runOnIdle {
            assertNotSame(originalMap, currentMap)
            assertFalse(ownership.value)
        }
    }

    @Test
    fun hiddenMapViewDoesNotReceiveTouchInput() {
        val revealed = mutableStateOf(false)
        var touchCount = 0

        compose.setContent {
            Box(Modifier.fillMaxSize().testTag("locator_host")) {
                StableAndroidViewHost(
                    revealed = revealed.value,
                    factory = { context ->
                        View(context).apply {
                            setOnTouchListener { _, _ ->
                                touchCount++
                                true
                            }
                        }
                    },
                    modifier = Modifier.fillMaxSize(),
                )
                if (!revealed.value) MapLocatingOverlay()
            }
        }

        compose.onNodeWithTag("locator_host").performTouchInput { click() }
        compose.runOnIdle { assertEquals(0, touchCount) }

        compose.runOnUiThread { revealed.value = true }
        compose.waitForIdle()
        compose.onNodeWithTag("locator_host").performTouchInput { click() }

        compose.runOnIdle { assertEquals(2, touchCount) }
    }

    @Test
    fun nativeTouchGateConsumesHiddenInputWithoutTakingCameraOwnership() {
        val view = View(androidx.test.platform.app.InstrumentationRegistry.getInstrumentation().targetContext)
        var revealed = false
        var ownershipClaims = 0
        view.installMapCameraTouchListener(
            isMapRevealed = { revealed },
            onUserTouch = { ownershipClaims++ },
        )
        val down = MotionEvent.obtain(0L, 0L, MotionEvent.ACTION_DOWN, 10f, 10f, 0)

        assertTrue(view.dispatchTouchEvent(down))
        assertEquals(0, ownershipClaims)

        revealed = true
        assertFalse(view.dispatchTouchEvent(down))
        assertEquals(1, ownershipClaims)
        down.recycle()
    }
}

private class TrackingView(
    context: Context,
    private val onDetached: () -> Unit,
) : View(context) {
    override fun onDetachedFromWindow() {
        onDetached()
        super.onDetachedFromWindow()
    }
}
