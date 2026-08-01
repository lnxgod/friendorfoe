package com.friendorfoe

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.presentation.privacy.PrivacyAlertBootstrap
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class FriendOrFoeApplication : Application(), DefaultLifecycleObserver {

    @Inject lateinit var badgeControlPort: BadgeControlPort
    @Inject lateinit var privacyAlertBootstrap: PrivacyAlertBootstrap

    override fun onCreate() {
        super<Application>.onCreate()

        privacyAlertBootstrap.start()
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
    }

    override fun onStart(owner: LifecycleOwner) {
        badgeControlPort.start()
    }

    override fun onStop(owner: LifecycleOwner) {
        badgeControlPort.stop()
    }
}
