package com.friendorfoe.di

import com.friendorfoe.presentation.ar.AndroidPhotoWriter
import com.friendorfoe.presentation.ar.AndroidShareImageFactory
import com.friendorfoe.presentation.ar.PhotoWriter
import com.friendorfoe.presentation.ar.ShareImageFactory
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent

@Module
@InstallIn(SingletonComponent::class)
abstract class CaptureModule {
    @Binds
    abstract fun bindPhotoWriter(implementation: AndroidPhotoWriter): PhotoWriter

    @Binds
    abstract fun bindShareImageFactory(implementation: AndroidShareImageFactory): ShareImageFactory
}
