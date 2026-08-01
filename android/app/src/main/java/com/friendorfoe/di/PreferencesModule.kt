package com.friendorfoe.di

import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.AppPreferencesRepository
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class PreferencesModule {

    @Binds
    @Singleton
    abstract fun bindAppPreferences(implementation: AppPreferencesRepository): AppPreferences
}
