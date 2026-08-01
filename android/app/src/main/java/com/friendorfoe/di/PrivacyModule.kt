package com.friendorfoe.di

import com.friendorfoe.presentation.privacy.BackendPrivacySourceAdapter
import com.friendorfoe.presentation.privacy.BadgePrivacySourceAdapter
import com.friendorfoe.presentation.privacy.PhonePrivacySourceAdapter
import com.friendorfoe.presentation.privacy.PrivacySourceAdapter
import com.friendorfoe.presentation.privacy.WifiPrivacySourceAdapter
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dagger.multibindings.IntoSet

@Module
@InstallIn(SingletonComponent::class)
abstract class PrivacyModule {
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
