package com.friendorfoe.presentation.detail

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.SkyObject
import javax.inject.Inject
import javax.inject.Singleton

interface DetailLookup {
    fun currentObject(objectId: String): SkyObject?
    suspend fun newestSnapshot(objectId: String): HistoryEntity?
    suspend fun snapshot(historyId: Long): HistoryEntity?
}

@Singleton
class RepositoryDetailLookup @Inject constructor(
    private val skyObjects: SkyObjectRepository,
    private val history: HistoryStore,
) : DetailLookup {
    override fun currentObject(objectId: String): SkyObject? =
        skyObjects.skyObjects.value.firstOrNull { it.id == objectId }

    override suspend fun newestSnapshot(objectId: String): HistoryEntity? =
        history.getNewestByObjectId(objectId)

    override suspend fun snapshot(historyId: Long): HistoryEntity? = history.getById(historyId)
}

class DetailSelectionLoader @Inject constructor(
    private val lookup: DetailLookup,
) {
    fun loadCurrent(objectId: String): SkyObject? = lookup.currentObject(objectId)

    suspend fun loadFallback(objectId: String): HistoryEntity? = lookup.newestSnapshot(objectId)

    suspend fun loadHistorical(historyId: Long): HistoryEntity? = lookup.snapshot(historyId)
}
