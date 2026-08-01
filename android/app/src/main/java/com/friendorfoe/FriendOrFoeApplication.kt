package com.friendorfoe

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.friendorfoe.data.badge.BadgeControlPort
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class FriendOrFoeApplication : Application(), DefaultLifecycleObserver {

    @Inject lateinit var badgeControlPort: BadgeControlPort

    override fun onCreate() {
        super<Application>.onCreate()

        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
    }

    override fun onStart(owner: LifecycleOwner) {
        badgeControlPort.start()
    }

    override fun onStop(owner: LifecycleOwner) {
        badgeControlPort.stop()
    }
}
