package com.friendorfoe.data.repository

import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
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
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val repository = EntryPointAccessors.fromApplication(
            context.applicationContext,
            SkyObjectRepositoryTestEntryPoint::class.java,
        ).skyObjectRepository()
        val permissions = MutableLocalDetectionPermissionProvider(LocalDetectionPermissions.None)
        val productionProvider = repository.replacePermissionProviderForTest(permissions)

        try {
            compose.setContent { Text("Collector lifecycle host") }
            repository.onRuntimePermissionsChanged()
            compose.waitUntil(timeoutMillis = 5_000) {
                repository.collectionJobForTest()?.isActive == true
            }
            val deniedPermissionJob = requireNotNull(repository.collectionJobForTest())

            permissions.current = LocalDetectionPermissions(
                bluetoothScan = true,
                wifiAwareScan = false,
                wifiManagerScanResults = false,
                audioCapture = false,
            )
            repository.onRuntimePermissionsChanged()

            compose.waitUntil(timeoutMillis = 5_000) {
                repository.collectionJobForTest()?.let {
                    it !== deniedPermissionJob && it.isActive
                } == true
            }
            val grantedPermissionJob = requireNotNull(repository.collectionJobForTest())
            assertTrue(deniedPermissionJob.isCancelled)
            assertNotSame(deniedPermissionJob, grantedPermissionJob)
            assertTrue(grantedPermissionJob.isActive)
        } finally {
            repository.replacePermissionProviderForTest(productionProvider)
            repository.onRuntimePermissionsChanged()
        }
    }
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface SkyObjectRepositoryTestEntryPoint {
    fun skyObjectRepository(): SkyObjectRepository
}

private class MutableLocalDetectionPermissionProvider(
    var current: LocalDetectionPermissions,
) : LocalDetectionPermissionProvider {
    override fun current(): LocalDetectionPermissions = current
}

private fun SkyObjectRepository.replacePermissionProviderForTest(
    replacement: LocalDetectionPermissionProvider,
): LocalDetectionPermissionProvider {
    val field = SkyObjectRepository::class.java.getDeclaredField("localDetectionPermissionProvider")
    field.isAccessible = true
    return (field.get(this) as LocalDetectionPermissionProvider).also {
        field.set(this, replacement)
    }
}

private fun SkyObjectRepository.collectionJobForTest(): Job? {
    val field = SkyObjectRepository::class.java.getDeclaredField("collectionJob")
    field.isAccessible = true
    return field.get(this) as? Job
}
