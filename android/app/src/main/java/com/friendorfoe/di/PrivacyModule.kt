package com.friendorfoe.di

import com.friendorfoe.presentation.privacy.BackendPrivacySourceAdapter
import com.friendorfoe.presentation.privacy.BadgePrivacySourceAdapter
import com.friendorfoe.presentation.privacy.PhonePrivacySourceAdapter
import com.friendorfoe.presentation.privacy.PrivacySourceAdapter
import com.friendorfoe.presentation.privacy.PrivacyAlertNotifier
import com.friendorfoe.presentation.privacy.PrivacyAlertPublisher
import com.friendorfoe.presentation.privacy.PrivacyNotificationIdStore
import com.friendorfoe.presentation.privacy.SharedPreferencesPrivacyNotificationIdStore
import com.friendorfoe.presentation.privacy.WifiPrivacySourceAdapter
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dagger.multibindings.IntoSet
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class PrivacyModule {
    @Binds
    @Singleton
    abstract fun bindPrivacyNotificationIdStore(
        store: SharedPreferencesPrivacyNotificationIdStore,
    ): PrivacyNotificationIdStore

    @Binds
    @Singleton
    abstract fun bindPrivacyAlertPublisher(
        notifier: PrivacyAlertNotifier,
    ): PrivacyAlertPublisher

    @Binds
    @IntoSet
    abstract fun bindPhonePrivacySourceAdapter(
        adapter: PhonePrivacySourceAdapter,
    ): PrivacySourceAdapter

    @Binds
    @IntoSet
    abstract fun bindBackendPrivacySourceAdapter(
        adapter: BackendPrivacySourceAdapter,
    ): PrivacySourceAdapter

    @Binds
    @IntoSet
    abstract fun bindBadgePrivacySourceAdapter(
        adapter: BadgePrivacySourceAdapter,
    ): PrivacySourceAdapter

    @Binds
    @IntoSet
    abstract fun bindWifiPrivacySourceAdapter(
        adapter: WifiPrivacySourceAdapter,
    ): PrivacySourceAdapter
}
