package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.repository.RuntimePermissionChangeNotifier
import com.friendorfoe.detection.IrCameraDetector
import org.junit.Assert.assertEquals
import org.junit.Test

class IrCameraScanViewModelTest {
    @Test
    fun permissionResultNotifiesLocalDetectionCollectors() {
        val notifier = RecordingRuntimePermissionChangeNotifier()
        val viewModel = IrCameraScanViewModel(
            irCameraDetector = IrCameraDetector(),
            permissionChangeNotifier = notifier,
        )

        viewModel.onRuntimePermissionsChanged()

        assertEquals(1, notifier.notificationCount)
    }
}

private class RecordingRuntimePermissionChangeNotifier : RuntimePermissionChangeNotifier {
    var notificationCount = 0

    override fun onRuntimePermissionsChanged() {
        notificationCount++
    }
}
