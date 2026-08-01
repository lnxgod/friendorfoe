package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.remote.AppUpdateApi
import com.friendorfoe.data.remote.BackendHealthClient
import com.friendorfoe.data.remote.HttpBackendHealthClient
import com.friendorfoe.data.repository.AppUpdateRepository
import com.friendorfoe.data.repository.HttpAppUpdateRepository
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
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory

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
        repository: HttpAppUpdateRepository,
    ): AppUpdateRepository

    companion object {
        private const val GITHUB_API_BASE_URL = "https://api.github.com/"

        @Provides
        @Singleton
        @Named("appUpdate")
        fun provideAppUpdateRetrofit(
            okHttpClient: OkHttpClient,
        ): Retrofit = Retrofit.Builder()
            .baseUrl(GITHUB_API_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()

        @Provides
        @Singleton
        fun provideAppUpdateApi(
            @Named("appUpdate") retrofit: Retrofit,
        ): AppUpdateApi = retrofit.create(AppUpdateApi::class.java)

        @Provides
        @Singleton
        fun provideBackendHealthClient(
            okHttpClient: OkHttpClient,
        ): BackendHealthClient = HttpBackendHealthClient(okHttpClient)

        @Provides
        @Singleton
        fun provideInstalledAppVersion(): AppVersion = AppVersion(
            code = BuildConfig.VERSION_CODE.toLong(),
            name = BuildConfig.VERSION_NAME,
        )
    }
}
