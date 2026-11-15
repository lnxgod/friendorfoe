package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.remote.BackendHealthClient
import com.friendorfoe.data.remote.HttpBackendHealthClient
import com.friendorfoe.data.repository.AppUpdateRepository
import com.friendorfoe.data.repository.PlayAppUpdateRepository
import com.friendorfoe.presentation.about.AndroidInfoSettingsStore
import com.friendorfoe.presentation.about.InfoSettingsStore
import dagger.Binds
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Named
import javax.inject.Singleton
import okhttp3.OkHttpClient

@Module
@InstallIn(SingletonComponent::class)
abstract class InfoModule {
    @Binds
    @Singleton
    abstract fun bindInfoSettingsStore(
        store: AndroidInfoSettingsStore,
    ): InfoSettingsStore

    @Binds
    @Singleton
    abstract fun bindAppUpdateRepository(
        repository: PlayAppUpdateRepository,
    ): AppUpdateRepository

    companion object {
        @Provides
        @Singleton
        fun provideBackendHealthClient(
            @Named("backendClient") okHttpClient: OkHttpClient,
        ): BackendHealthClient = HttpBackendHealthClient(okHttpClient)

        @Provides
        @Singleton
        fun provideInstalledAppVersion(): AppVersion = AppVersion(
            code = BuildConfig.VERSION_CODE.toLong(),
            name = BuildConfig.VERSION_NAME,
        )
    }
}
