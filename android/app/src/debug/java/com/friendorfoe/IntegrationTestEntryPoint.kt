package com.friendorfoe

import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.presentation.watch.WatchModeController
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent

/** Exposes real app singletons to instrumentation; excluded from release builds. */
@EntryPoint
@InstallIn(SingletonComponent::class)
interface IntegrationTestEntryPoint {
    fun skyObjectRepository(): SkyObjectRepository
    fun watchController(): WatchModeController
}
