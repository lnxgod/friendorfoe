package com.friendorfoe.di

import com.friendorfoe.data.time.AndroidMonotonicClock
import com.friendorfoe.data.time.MonotonicClock
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class BadgeModule {
    @Binds
    @Singleton
    abstract fun bindMonotonicClock(clock: AndroidMonotonicClock): MonotonicClock
}
