package com.friendorfoe.data.repository

import com.friendorfoe.data.local.FriendOrFoeDatabase
import com.friendorfoe.data.local.SavedFlightEntity
import com.friendorfoe.data.local.TrackingEntity
import com.google.gson.Gson
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class SavedFlightRepository @Inject constructor(database: FriendOrFoeDatabase) {
    private val dao = database.savedFlightDao()
    fun observeAll() = dao.observeAll()
    fun observe(id: String) = dao.observe(id)
    suspend fun delete(id: String) = dao.delete(id)
    suspend fun save(objectId: String, label: String, points: List<TrackingEntity>): String {
        val valid = points.filter { validTrackPoint(it) && it.objectId == objectId }.sortedBy { it.timestamp }
        require(valid.isNotEmpty()) { "No received positions to save" }
        val id = UUID.randomUUID().toString()
        dao.insert(SavedFlightEntity(id, objectId, label, System.currentTimeMillis(), valid.size, Gson().toJson(valid)))
        return id
    }
}

fun SavedFlightEntity.decodedPoints(): List<TrackingEntity> =
    Gson().fromJson(pointsJson, Array<TrackingEntity>::class.java).toList().filter(::validTrackPoint).sortedBy { it.timestamp }
