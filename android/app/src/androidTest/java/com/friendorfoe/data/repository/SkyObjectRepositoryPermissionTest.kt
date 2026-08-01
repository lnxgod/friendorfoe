package com.friendorfoe.data.repository

import android.Manifest
import android.content.pm.PackageManager
import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.core.content.ContextCompat
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.FriendOrFoeApplication
import kotlinx.coroutines.Job
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class SkyObjectRepositoryPermissionTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun permissionChangeCancelsAndReplacesTheRealCollectorJob() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_DENIED
        )
        assertTrue(
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_DENIED
        )

        compose.setContent { Text("Collector lifecycle host") }
        val repository = (context.applicationContext as FriendOrFoeApplication).skyObjectRepository
        compose.waitUntil(timeoutMillis = 5_000) { repository.collectionJobForTest()?.isActive == true }
        val deniedPermissionJob = requireNotNull(repository.collectionJobForTest())

        instrumentation.uiAutomation.grantRuntimePermission(
            context.packageName,
            Manifest.permission.BLUETOOTH_SCAN,
        )
        instrumentation.uiAutomation.grantRuntimePermission(
            context.packageName,
            Manifest.permission.BLUETOOTH_CONNECT,
        )
        repository.onRuntimePermissionsChanged()

        compose.waitUntil(timeoutMillis = 5_000) {
            repository.collectionJobForTest()?.let { it !== deniedPermissionJob && it.isActive } == true
        }
        val grantedPermissionJob = requireNotNull(repository.collectionJobForTest())
        assertTrue(deniedPermissionJob.isCancelled)
        assertNotSame(deniedPermissionJob, grantedPermissionJob)
        assertTrue(grantedPermissionJob.isActive)
    }
}

private fun SkyObjectRepository.collectionJobForTest(): Job? {
    val field = SkyObjectRepository::class.java.getDeclaredField("collectionJob")
    field.isAccessible = true
    return field.get(this) as? Job
}
