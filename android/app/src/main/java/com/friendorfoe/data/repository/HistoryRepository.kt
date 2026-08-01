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
) : HistoryStore {

    /** Get all history entries as a reactive Flow. */
    override fun observeAll(): Flow<List<HistoryEntity>> = historyDao.getAllHistory()

    /** Get history filtered by object type. */
    override fun observeByType(objectType: String): Flow<List<HistoryEntity>> =
        historyDao.getHistoryByType(objectType)

    override suspend fun getById(id: Long): HistoryEntity? = historyDao.getById(id)

    override suspend fun getNewestByObjectId(objectId: String): HistoryEntity? =
        historyDao.getByObjectId(objectId)

    /** Save a detection to history. */
    override suspend fun save(entity: HistoryEntity): Long = historyDao.insert(entity)

    override suspend fun deleteById(id: Long) = historyDao.deleteById(id)

    /** Delete all history entries. */
    override suspend fun clearAll() = historyDao.deleteAll()

    /** Delete history older than a given timestamp. */
    override suspend fun prune(beforeTimeMillis: Long) = historyDao.deleteOlderThan(beforeTimeMillis)
}
