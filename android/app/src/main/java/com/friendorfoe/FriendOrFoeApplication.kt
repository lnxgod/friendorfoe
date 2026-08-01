package com.friendorfoe

import android.app.Application
import android.util.Log
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.presentation.privacy.PrivacyAlertBootstrap
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class FriendOrFoeApplication : Application(), DefaultLifecycleObserver {

    @Inject lateinit var badgeUsbRepository: BadgeUsbRepository
    @Inject lateinit var privacyAlertBootstrap: PrivacyAlertBootstrap
    @Inject lateinit var skyObjectRepository: SkyObjectRepository

    override fun onCreate() {
        super<Application>.onCreate()

        privacyAlertBootstrap.start()
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
    }

    override fun onStart(owner: LifecycleOwner) {
        Log.i("FriendOrFoeApp", "App foregrounded — restarting detection sources")
        skyObjectRepository.ensureStarted(0.0, 0.0)
        badgeUsbRepository.start()
    }

    override fun onStop(owner: LifecycleOwner) {
        Log.i("FriendOrFoeApp", "App backgrounded — stopping scanning to save battery")
        skyObjectRepository.stop()
        badgeUsbRepository.stop()
    }
}
