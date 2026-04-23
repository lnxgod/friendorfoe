package com.friendorfoe.di

import com.friendorfoe.calibration.AndroidCalibrationPlatform
import com.friendorfoe.calibration.BleCalibrationAdvertiser
import com.friendorfoe.calibration.CalibrationAdvertiser
import com.friendorfoe.calibration.CalibrationApi
import com.friendorfoe.calibration.CalibrationBackend
import com.friendorfoe.calibration.CalibrationPlatform
import com.friendorfoe.calibration.CalibrationSettingsStore
import com.friendorfoe.data.DetectionPrefs
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class CalibrationModule {

    @Binds
    @Singleton
    abstract fun bindCalibrationSettingsStore(impl: DetectionPrefs): CalibrationSettingsStore

    @Binds
    @Singleton
    abstract fun bindCalibrationBackend(impl: CalibrationApi): CalibrationBackend

    @Binds
    @Singleton
    abstract fun bindCalibrationAdvertiser(impl: BleCalibrationAdvertiser): CalibrationAdvertiser

    @Binds
    @Singleton
    abstract fun bindCalibrationPlatform(impl: AndroidCalibrationPlatform): CalibrationPlatform
}
