package com.friendorfoe.data.repository

import com.friendorfoe.data.local.HistoryDao
import com.friendorfoe.data.local.HistoryEntity
import kotlinx.coroutines.flow.Flow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Repository for detection history.
 *
 * Wraps the Room DAO to provide a clean API for use cases.
 */
@Singleton
class HistoryRepository @Inject constructor(
    private val historyDao: HistoryDao
) {

    /** Get all history entries as a reactive Flow. */
    fun getAllHistory(): Flow<List<HistoryEntity>> {
        return historyDao.getAllHistory()
    }

    /** Get history filtered by object type. */
    fun getHistoryByType(objectType: String): Flow<List<HistoryEntity>> {
        return historyDao.getHistoryByType(objectType)
    }

    /** Save a detection to history. */
    suspend fun saveDetection(entity: HistoryEntity): Long {
        return historyDao.insert(entity)
    }

    /** Delete all history entries. */
    suspend fun clearHistory() {
        historyDao.deleteAll()
    }

    /** Delete history older than a given timestamp. */
    suspend fun pruneHistory(beforeTimeMillis: Long) {
        historyDao.deleteOlderThan(beforeTimeMillis)
    }
}
