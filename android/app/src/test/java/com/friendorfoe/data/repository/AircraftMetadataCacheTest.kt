package com.friendorfoe.data.repository

import com.friendorfoe.data.remote.AircraftDetailDto
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class AircraftMetadataCacheTest {

    @Test
    fun successCacheReusesMetadataRegardlessOfHexCase() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(nowMs = { nowMs })

        val first = cache.getOrLoad("ABC123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Found(detail("abc123"))
        }
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Fresh positive cache entry should be reused")
        }

        assertSame(first, second)
        assertEquals(1, loadCount)
    }

    @Test
    fun notFoundCacheSuppressesRepeatedLookupsUntilMissTtlExpires() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(
            missTtlMs = 10_000L,
            nowMs = { nowMs }
        )

        val first = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.NotFound
        }
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Fresh miss cache entry should be reused")
        }
        nowMs += 10_001L
        val third = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Found(detail("abc123"))
        }

        assertNull(first)
        assertNull(second)
        assertEquals("N12345", third!!.registration)
        assertEquals(2, loadCount)
    }

    @Test
    fun errorCacheSuppressesRepeatedLookupsOnlyForShortErrorTtl() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(
            errorTtlMs = 5_000L,
            nowMs = { nowMs }
        )

        val first = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Failed
        }
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Fresh error cache entry should be reused")
        }
        nowMs += 5_001L
        val third = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Found(detail("abc123"))
        }

        assertNull(first)
        assertNull(second)
        assertEquals("N12345", third!!.registration)
        assertEquals(2, loadCount)
    }

    @Test
    fun defaultSuccessCacheKeepsMetadataForOneDay() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(nowMs = { nowMs })

        val first = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Found(detail("abc123"))
        }
        nowMs += 24 * 60 * 60 * 1000L
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Default positive cache should survive a full day")
        }

        assertSame(first, second)
        assertEquals(1, loadCount)
    }

    @Test
    fun defaultMissCacheKeepsNotFoundForHalfDay() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(nowMs = { nowMs })

        val first = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.NotFound
        }
        nowMs += 12 * 60 * 60 * 1000L
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Default miss cache should survive half a day")
        }

        assertNull(first)
        assertNull(second)
        assertEquals(1, loadCount)
    }

    @Test
    fun defaultErrorCacheKeepsFailuresForFifteenMinutes() = runTest {
        var nowMs = 1_000L
        var loadCount = 0
        val cache = AircraftMetadataCache(nowMs = { nowMs })

        val first = cache.getOrLoad("abc123") {
            loadCount += 1
            AircraftMetadataCache.LookupResult.Failed
        }
        nowMs += 15 * 60 * 1000L
        val second = cache.getOrLoad("abc123") {
            loadCount += 1
            error("Default error cache should survive fifteen minutes")
        }

        assertNull(first)
        assertNull(second)
        assertEquals(1, loadCount)
    }

    @Test
    fun defaultCacheKeepsMoreThanFiveHundredAircraft() = runTest {
        var nowMs = 1_000L
        val cache = AircraftMetadataCache(nowMs = { nowMs })

        repeat(501) { index ->
            val hex = index.toString(16).padStart(6, '0')
            cache.getOrLoad(hex) {
                AircraftMetadataCache.LookupResult.Found(detail(hex))
            }
            nowMs += 1_000L
        }

        val first = cache.getOrLoad("000000") {
            error("Default metadata cache should retain more than 500 aircraft")
        }

        assertEquals("000000", first!!.icaoHex)
    }

    @Test
    fun concurrentRequestsForSameHexShareOneProviderCall() = runTest {
        var loadCount = 0
        val started = CompletableDeferred<Unit>()
        val unblock = CompletableDeferred<Unit>()
        val cache = AircraftMetadataCache()

        val first = async {
            cache.getOrLoad("ABC123") {
                loadCount += 1
                started.complete(Unit)
                unblock.await()
                AircraftMetadataCache.LookupResult.Found(detail("abc123"))
            }
        }
        started.await()
        val second = async {
            cache.getOrLoad("abc123") {
                loadCount += 1
                error("Concurrent request should join the first lookup")
            }
        }
        unblock.complete(Unit)

        assertSame(first.await(), second.await())
        assertEquals(1, loadCount)
    }

    private fun detail(hex: String) = AircraftDetailDto(
        icaoHex = hex,
        callsign = null,
        registration = "N12345",
        aircraftType = "AS50",
        aircraftDescription = "Eurocopter AS350",
        operator = "COUNTY SHERIFF",
        photo = null,
        route = null,
        country = null
    )
}
