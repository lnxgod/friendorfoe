package com.friendorfoe

import android.app.Application
import android.util.Log
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class FriendOrFoeApplication : Application() {

    @Inject lateinit var skyObjectRepository: SkyObjectRepository

    override fun onCreate() {
        super.onCreate()

        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStop(owner: LifecycleOwner) {
                Log.i("FriendOrFoeApp", "App backgrounded — stopping scanning to save battery")
                skyObjectRepository.stop()
            }
        })
    }
}
