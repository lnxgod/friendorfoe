package com.friendorfoe.data.repository

import com.friendorfoe.data.local.TrackingDao
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.domain.model.Aircraft
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

@Singleton
class AircraftTrackRepository @Inject constructor(
    private val dao: TrackingDao,
    private val clock: MonotonicClock,
) {
    private val recordingMutex = Mutex()
    private var lastPrunedAtMs: Long? = null

    fun observeTrail(objectId: String): Flow<List<TrackingEntity>> = combine(
        dao.observeTrailForObject(objectId, MAX_AIRCRAFT_TRACK_POINTS),
        clock.ticks(60_000L),
    ) { points, _ ->
        val now = clock.nowWallClock().toEpochMilli()
        points.filter { validTrackPoint(it) && it.timestamp in (now - AIRCRAFT_TRACK_RETENTION_MS)..now }
    }.distinctUntilChanged()

    suspend fun record(aircraft: List<Aircraft>) = recordingMutex.withLock {
        val now = clock.nowWallClock().toEpochMilli()
        val prune = lastPrunedAtMs?.let { now < it || now - it >= 300_000L } ?: true
        val points = aircraft.map { item ->
            TrackingEntity(
                objectId = item.id,
                latitude = item.position.latitude,
                longitude = item.position.longitude,
                altitudeMeters = item.position.altitudeMeters,
                heading = item.position.heading,
                speedMps = item.position.speedMps,
                timestamp = item.lastUpdated.toEpochMilli(),
            )
        }
        dao.recordAircraftBatch(points, now, prune)
        if (prune) lastPrunedAtMs = now
    }
}
