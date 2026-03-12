package com.friendorfoe

import android.app.Application
import dagger.hilt.android.HiltAndroidApp

/**
 * Application class for Friend or Foe.
 *
 * @HiltAndroidApp triggers Hilt's code generation, including a base class
 * for the application that serves as the application-level dependency container.
 */
@HiltAndroidApp
class FriendOrFoeApplication : Application() {

    override fun onCreate() {
        super.onCreate()
        // Future: initialize crash reporting, logging, etc.
    }
}
