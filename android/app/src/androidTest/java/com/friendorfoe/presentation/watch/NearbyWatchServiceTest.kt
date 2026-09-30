package com.friendorfoe.presentation.watch

import android.Manifest
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.core.app.ApplicationProvider
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.presentation.MainActivity
import com.friendorfoe.IntegrationTestEntryPoint
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Test


class NearbyWatchServiceTest {
    @Test fun explicitWatchSurvivesBackgroundAndNotificationStopEndsIt() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val permissions = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION); add(Manifest.permission.ACCESS_COARSE_LOCATION)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }
        permissions.forEach {
            instrumentation.uiAutomation.executeShellCommand("pm grant ${context.packageName} $it").use { fd ->
                android.os.ParcelFileDescriptor.AutoCloseInputStream(fd).readBytes()
            }
        }
        instrumentation.uiAutomation.executeShellCommand("cmd location set-location-enabled true").close()
        val watch = EntryPointAccessors.fromApplication(context, IntegrationTestEntryPoint::class.java).watchController()
        val manager = context.getSystemService(NotificationManager::class.java)
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            try {
                scenario.onActivity { watch.start() }
                withTimeout(10_000) { watch.state.first { it.active } }
                scenario.moveToState(Lifecycle.State.CREATED)
                kotlinx.coroutines.delay(1500) // Allow ProcessLifecycleOwner's background transition.
                assertTrue(watch.state.value.active)
                val notification = manager.activeNotifications.single { it.id == NearbyWatchService.ID }.notification
                notification.actions.single { it.title.toString() == "Stop watching" }.actionIntent.send()
                withTimeout(10_000) { watch.state.first { !it.active && !it.starting } }
                assertFalse(manager.activeNotifications.any { it.id == NearbyWatchService.ID })
            } finally {
                context.stopService(Intent(context, NearbyWatchService::class.java))
                scenario.moveToState(Lifecycle.State.RESUMED)
            }
        }
    }
}
