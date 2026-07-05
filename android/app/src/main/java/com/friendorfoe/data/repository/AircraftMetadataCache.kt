package com.friendorfoe.data.repository

import com.friendorfoe.data.remote.AircraftDetailDto
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.coroutineScope
import java.util.concurrent.ConcurrentHashMap

internal class AircraftMetadataCache(
    private val maxSize: Int = DEFAULT_MAX_SIZE,
    private val successTtlMs: Long = SUCCESS_TTL_MS,
    private val missTtlMs: Long = MISS_TTL_MS,
    private val errorTtlMs: Long = ERROR_TTL_MS,
    private val nowMs: () -> Long = System::currentTimeMillis
) {

    sealed class LookupResult {
        data class Found(val detail: AircraftDetailDto) : LookupResult()
        object NotFound : LookupResult()
        object Failed : LookupResult()
    }

    private enum class Status {
        SUCCESS,
        MISS,
        ERROR
    }

    private data class Entry(
        val detail: AircraftDetailDto?,
        val status: Status,
        val timestampMs: Long
    )

    private val entries = ConcurrentHashMap<String, Entry>()
    private val inFlight = ConcurrentHashMap<String, Deferred<AircraftDetailDto?>>()

    suspend fun getOrLoad(
        icaoHex: String,
        load: suspend (String) -> LookupResult
    ): AircraftDetailDto? = coroutineScope {
        val key = normalizeKey(icaoHex)
        freshEntry(key)?.let { return@coroutineScope it.detail }

        val deferred = async(start = CoroutineStart.LAZY) {
            val result = try {
                load(key)
            } catch (_: Exception) {
                LookupResult.Failed
            }
            val entry = result.toEntry(nowMs())
            store(key, entry)
            entry.detail
        }
        val existing = inFlight.putIfAbsent(key, deferred)
        if (existing != null) {
            deferred.cancel()
            return@coroutineScope existing.await()
        }

        try {
            deferred.start()
            deferred.await()
        } finally {
            inFlight.remove(key, deferred)
        }
    }

    private fun freshEntry(key: String): Entry? {
        val entry = entries[key] ?: return null
        val ageMs = nowMs() - entry.timestampMs
        val ttlMs = when (entry.status) {
            Status.SUCCESS -> successTtlMs
            Status.MISS -> missTtlMs
            Status.ERROR -> errorTtlMs
        }
        return if (ageMs <= ttlMs) {
            entry
        } else {
            entries.remove(key, entry)
            null
        }
    }

    private fun store(key: String, entry: Entry) {
        entries[key] = entry
        pruneToMaxSize()
    }

    private fun pruneToMaxSize() {
        while (entries.size > maxSize) {
            val oldest = entries.entries.minByOrNull { it.value.timestampMs } ?: return
            entries.remove(oldest.key, oldest.value)
        }
    }

    private fun LookupResult.toEntry(timestampMs: Long): Entry = when (this) {
        is LookupResult.Found -> Entry(detail = detail, status = Status.SUCCESS, timestampMs = timestampMs)
        LookupResult.NotFound -> Entry(detail = null, status = Status.MISS, timestampMs = timestampMs)
        LookupResult.Failed -> Entry(detail = null, status = Status.ERROR, timestampMs = timestampMs)
    }

    private fun normalizeKey(icaoHex: String): String =
        icaoHex.trim().lowercase()

    companion object {
        private const val DEFAULT_MAX_SIZE = 2_500
        private const val MINUTE_MS = 60 * 1000L
        private const val HOUR_MS = 60 * MINUTE_MS
        private const val SUCCESS_TTL_MS = 24 * HOUR_MS
        private const val MISS_TTL_MS = 12 * HOUR_MS
        private const val ERROR_TTL_MS = 15 * MINUTE_MS
    }
}
