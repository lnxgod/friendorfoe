package com.friendorfoe.di

import android.content.Context
import com.friendorfoe.data.local.FriendOrFoeDatabase
import com.friendorfoe.data.local.HistoryDao
import com.friendorfoe.data.local.TrackingDao
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

/**
 * Hilt module providing local database dependencies (Room database, DAOs).
 */
@Module
@InstallIn(SingletonComponent::class)
object DatabaseModule {

    @Provides
    @Singleton
    fun provideDatabase(@ApplicationContext context: Context): FriendOrFoeDatabase {
        return FriendOrFoeDatabase.create(context)
    }

    @Provides
    @Singleton
    fun provideHistoryDao(database: FriendOrFoeDatabase): HistoryDao {
        return database.historyDao()
    }

    @Provides
    @Singleton
    fun provideTrackingDao(database: FriendOrFoeDatabase): TrackingDao {
        return database.trackingDao()
    }
}
