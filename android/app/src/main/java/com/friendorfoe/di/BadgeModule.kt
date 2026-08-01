package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.data.badge.BadgeDebugBridgeConfig
import com.friendorfoe.data.badge.BadgeHttpClients
import com.friendorfoe.data.badge.BadgeReleaseCertification
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.data.badge.CheckedInBadgeReleaseCertification
import com.friendorfoe.data.badge.badgeDebugBridgeConfig
import com.friendorfoe.data.badge.badgeHttpClients
import com.friendorfoe.data.time.AndroidMonotonicClock
import com.friendorfoe.data.time.MonotonicClock
import dagger.Binds
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton
import okhttp3.OkHttpClient

@Module
@InstallIn(SingletonComponent::class)
abstract class BadgeModule {
    @Binds
    @Singleton
    abstract fun bindBadgeControlPort(repository: BadgeUsbRepository): BadgeControlPort

    @Binds
    @Singleton
    abstract fun bindMonotonicClock(clock: AndroidMonotonicClock): MonotonicClock

    companion object {
        @Provides
        @Singleton
        fun provideBadgeReleaseCertification(): BadgeReleaseCertification =
            CheckedInBadgeReleaseCertification

        @Provides
        @Singleton
        fun provideBadgeDebugBridgeConfig(): BadgeDebugBridgeConfig = badgeDebugBridgeConfig(
            isDebug = BuildConfig.DEBUG,
            configuredUrl = BuildConfig.BADGE_DEBUG_BRIDGE_BASE_URL,
        )

        @Provides
        @Singleton
        fun provideBadgeHttpClients(base: OkHttpClient): BadgeHttpClients =
            badgeHttpClients(base)
    }
}
