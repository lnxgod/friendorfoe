package com.friendorfoe.data.time

import android.os.SystemClock
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.isActive
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

interface MonotonicClock {
    fun nowElapsedMs(): Long
    fun nowWallClock(): Instant
    fun ticks(periodMs: Long = 1_000): Flow<Long>
}

@Singleton
class AndroidMonotonicClock @Inject constructor() : MonotonicClock {
    override fun nowElapsedMs(): Long = SystemClock.elapsedRealtime()

    override fun nowWallClock(): Instant = Instant.now()

    override fun ticks(periodMs: Long): Flow<Long> = flow {
        while (currentCoroutineContext().isActive) {
            emit(nowElapsedMs())
            delay(periodMs)
        }
    }
}
