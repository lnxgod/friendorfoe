package com.friendorfoe.di

import com.friendorfoe.data.repository.HistoryRepository
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.presentation.detail.DetailLookup
import com.friendorfoe.presentation.detail.RepositoryDetailLookup
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent

@Module
@InstallIn(SingletonComponent::class)
abstract class HistoryModule {
    @Binds
    abstract fun bindHistoryStore(implementation: HistoryRepository): HistoryStore

    @Binds
    abstract fun bindDetailLookup(implementation: RepositoryDetailLookup): DetailLookup
}
