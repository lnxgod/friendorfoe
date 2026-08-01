package com.friendorfoe.di

import com.friendorfoe.detection.AndroidBleGattInspector
import com.friendorfoe.detection.BleInvestigator
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class BleInvestigationModule {
    @Binds
    @Singleton
    abstract fun bindBleInvestigator(impl: AndroidBleGattInspector): BleInvestigator
}
