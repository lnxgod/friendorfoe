package com.friendorfoe.presentation.map

import android.content.Context
import android.view.View
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.junit4.createComposeRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
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
