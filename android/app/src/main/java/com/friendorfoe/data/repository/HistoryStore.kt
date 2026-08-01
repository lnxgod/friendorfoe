package com.friendorfoe.data.repository

import com.friendorfoe.data.local.HistoryEntity
import kotlinx.coroutines.flow.Flow

interface HistoryStore {
    fun observeAll(): Flow<List<HistoryEntity>>
    fun observeByType(objectType: String): Flow<List<HistoryEntity>>
    suspend fun getById(id: Long): HistoryEntity?
    suspend fun getNewestByObjectId(objectId: String): HistoryEntity?
    suspend fun save(entity: HistoryEntity): Long
    suspend fun deleteById(id: Long)
    suspend fun clearAll()
    suspend fun prune(beforeTimeMillis: Long)
}
