package com.friendorfoe.data.repository

import android.util.Log
import com.friendorfoe.data.remote.AdsbFiApiService
import com.friendorfoe.data.remote.AdsbLolApiService
import com.friendorfoe.data.remote.AdsbxAircraft
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.data.remote.AirplanesLiveApiService
import com.friendorfoe.data.remote.OpenSkyApiService
import com.friendorfoe.detection.MilitaryClassifier
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import retrofit2.HttpException
import java.io.IOException
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.cos

/**
 * Result wrapper for nearby aircraft, indicating data source.
 */
data class NearbyResult(
    val aircraft: List<Aircraft>,
    val source: DataSource
)

enum class DataSource {
    ADSBFI, AIRPLANES_LIVE, OPENSKY, ADSB_LOL, MULTI
}

/**
 * Fetch errors with specific handling for rate limiting.
 */
sealed class FetchException(message: String, cause: Throwable? = null) : Exception(message, cause) {
    class RateLimited(val retryAfterSeconds: Int) : FetchException("Rate limited, retry after ${retryAfterSeconds}s")
    class HttpError(val code: Int, val msg: String) : FetchException("HTTP $code: $msg")
    class NetworkError(cause: Throwable) : FetchException("Network error: ${cause.message}", cause)
}

/**
 * Repository for aircraft data.
 *
 * Tries the backend API first (cached, enriched data), falling back to
 * OpenSky Network direct API on failure.
 */
@Singleton
class AircraftRepository @Inject constructor(
    private val openSkyApi: OpenSkyApiService,
    private val adsbFiApi: AdsbFiApiService,
    private val airplanesLiveApi: AirplanesLiveApiService,
    private val adsbLolApi: AdsbLolApiService
) {

    companion object {
        private const val TAG = "AircraftRepository"
        private const val NM_TO_KM = 1.852
        private const val KM_PER_DEG_LAT = 111.0
        private const val FT_TO_M = 0.3048
        private const val KTS_TO_MPS = 0.5144f
        private const val NM_TO_M = 1852.0
        private const val DETAIL_CACHE_TTL_MS = 10 * 60 * 1000L // 10 minutes
        private const val DETAIL_CACHE_MAX_SIZE = 100
    }

    private data class CachedDetail(
        val detail: AircraftDetailDto,
        val timestampMs: Long = System.currentTimeMillis()
    ) {
        fun isExpired(): Boolean =
            System.currentTimeMillis() - timestampMs > DETAIL_CACHE_TTL_MS
    }

    private val detailCache = ConcurrentHashMap<String, CachedDetail>()

    /**
     * Fetch nearby aircraft from all ADS-B providers in parallel and merge results.
     *
     * Queries adsb.fi, airplanes.live, adsb.lol, and OpenSky concurrently.
     * Merges results by ICAO hex, preferring the most recently updated aircraft
     * when duplicates exist. Returns MULTI source when multiple providers contributed.
     * Falls back to Result.failure only if ALL providers fail.
     */
    suspend fun getNearbyAircraft(
        latitude: Double,
        longitude: Double,
        radiusNm: Int = 50
    ): Result<NearbyResult> = coroutineScope {
        val adsbFiDeferred = async { fetchAdsbFi(latitude, longitude, radiusNm) }
        val airplanesLiveDeferred = async { fetchAirplanesLive(latitude, longitude, radiusNm) }
        val adsbLolDeferred = async { fetchAdsbLol(latitude, longitude, radiusNm) }
        val openSkyDeferred = async { fetchOpenSky(latitude, longitude, radiusNm) }

        val results = listOf(
            adsbFiDeferred.await(),
            airplanesLiveDeferred.await(),
            adsbLolDeferred.await(),
            openSkyDeferred.await()
        )

        val successful = results.filterNotNull()
        if (successful.isEmpty()) {
            Log.w(TAG, "All ADS-B sources failed")
            return@coroutineScope Result.failure(FetchException.NetworkError(IOException("All ADS-B sources failed")))
        }

        // Merge by ICAO hex, preferring the most recently updated aircraft
        val merged = mutableMapOf<String, Aircraft>()
        val contributingSources = mutableSetOf<DataSource>()
        for ((aircraft, source) in successful) {
            contributingSources.add(source)
            for (ac in aircraft) {
                val existing = merged[ac.icaoHex]
                if (existing == null || ac.lastUpdated.isAfter(existing.lastUpdated)) {
                    merged[ac.icaoHex] = ac
                }
            }
        }

        val finalSource = if (contributingSources.size > 1) DataSource.MULTI else contributingSources.first()
        Log.d(TAG, "Multi-source merged: ${merged.size} aircraft from ${contributingSources.joinToString()}")
        Result.success(NearbyResult(aircraft = merged.values.toList(), source = finalSource))
    }

    private suspend fun fetchAdsbFi(lat: Double, lon: Double, radiusNm: Int): Pair<List<Aircraft>, DataSource>? {
        return try {
            val response = adsbFiApi.getNearby(lat, lon, radiusNm)
            val aircraft = response.ac?.mapNotNull { it.toAircraft() } ?: emptyList()
            Log.d(TAG, "adsb.fi returned ${aircraft.size} aircraft")
            aircraft to DataSource.ADSBFI
        } catch (e: Exception) {
            Log.w(TAG, "adsb.fi failed: ${e.message}")
            null
        }
    }

    private suspend fun fetchAirplanesLive(lat: Double, lon: Double, radiusNm: Int): Pair<List<Aircraft>, DataSource>? {
        return try {
            val response = airplanesLiveApi.getNearby(lat, lon, radiusNm)
            val aircraft = response.ac?.mapNotNull { it.toAircraft() } ?: emptyList()
            Log.d(TAG, "airplanes.live returned ${aircraft.size} aircraft")
            aircraft to DataSource.AIRPLANES_LIVE
        } catch (e: Exception) {
            Log.w(TAG, "airplanes.live failed: ${e.message}")
            null
        }
    }

    private suspend fun fetchAdsbLol(lat: Double, lon: Double, radiusNm: Int): Pair<List<Aircraft>, DataSource>? {
        return try {
            val response = adsbLolApi.getNearby(lat, lon, radiusNm)
            val aircraft = response.ac?.mapNotNull { it.toAircraft() } ?: emptyList()
            Log.d(TAG, "adsb.lol returned ${aircraft.size} aircraft")
            aircraft to DataSource.ADSB_LOL
        } catch (e: Exception) {
            Log.w(TAG, "adsb.lol failed: ${e.message}")
            null
        }
    }

    private suspend fun fetchOpenSky(lat: Double, lon: Double, radiusNm: Int): Pair<List<Aircraft>, DataSource>? {
        return try {
            val radiusKm = radiusNm * NM_TO_KM
            val deltaLat = radiusKm / KM_PER_DEG_LAT
            val deltaLon = radiusKm / (KM_PER_DEG_LAT * cos(Math.toRadians(lat)))

            val response = openSkyApi.getStates(
                latMin = lat - deltaLat,
                latMax = lat + deltaLat,
                lonMin = lon - deltaLon,
                lonMax = lon + deltaLon
            )

            val aircraft = response.states?.mapNotNull { state ->
                parseStateVector(state)
            } ?: emptyList()

            Log.d(TAG, "OpenSky returned ${aircraft.size} aircraft")
            aircraft to DataSource.OPENSKY
        } catch (e: Exception) {
            Log.w(TAG, "OpenSky failed: ${e.message}")
            null
        }
    }

    /**
     * Fetch detailed info for a specific aircraft.
     * Backend has been removed — returns failure. Detail enrichment gracefully degrades.
     */
    suspend fun getAircraftDetail(icaoHex: String): Result<AircraftDetailDto> {
        // Check cache first (entries from previous sessions may still be valid)
        detailCache[icaoHex]?.let { cached ->
            if (!cached.isExpired()) {
                Log.d(TAG, "Detail cache hit for $icaoHex")
                return Result.success(cached.detail)
            } else {
                detailCache.remove(icaoHex)
            }
        }

        // No backend available — detail enrichment gracefully degrades
        return Result.failure(Exception("Backend not available"))
    }

    /**
     * Convert an ADSBx v2 format aircraft (adsb.fi / airplanes.live) to an Aircraft domain object.
     * Converts units: alt_baro ft→m, gs knots→m/s, baro_rate fpm→m/s.
     */
    private fun AdsbxAircraft.toAircraft(): Aircraft? {
        val icao = hex?.trim()?.lowercase() ?: return null
        if (lat == null || lon == null) return null

        // alt_baro can be Int or "ground"
        val altitudeM = when (altBaro) {
            is Number -> (altBaro as Number).toDouble() * FT_TO_M
            "ground", "Ground" -> 0.0
            else -> altGeom?.let { it.toDouble() * FT_TO_M } ?: 0.0
        }
        val onGround = altBaro == "ground" || altBaro == "Ground"
        val cleanCallsign = flight?.trim()?.ifBlank { null }

        val (classifiedCategory, signals) = classifyAircraft(
            hex = icao,
            callsign = cleanCallsign,
            typeCode = t,
            registration = r,
            adsbCategory = category,
            squawk = squawk
        )

        return Aircraft(
            id = icao,
            position = Position(
                latitude = lat,
                longitude = lon,
                altitudeMeters = altitudeM,
                heading = track?.toFloat(),
                speedMps = gs?.let { (it * KTS_TO_MPS).toFloat() }
            ),
            category = classifiedCategory,
            firstSeen = Instant.now(),
            lastUpdated = Instant.now(),
            icaoHex = icao,
            callsign = cleanCallsign,
            registration = r,
            aircraftType = t,
            squawk = squawk,
            isOnGround = onGround,
            classificationSignals = signals.ifEmpty { null }
        )
    }

    /** Cargo airline callsign prefixes (ICAO 3-letter codes). */
    private val cargoCallsignPrefixes = setOf(
        "FDX", "UPS", "GTI", "ABW", "CLX", "ABX", "ATN",
        "KAL", "CKK", "SQC", "CAO", "AIJ", "GEC", "ETD"
    )

    /** Emergency medevac/lifeguard callsign patterns. */
    private val emergencyCallsignPattern = Regex("^(LIFEGUARD|MEDEVAC).*", RegexOption.IGNORE_CASE)

    /**
     * Enhanced classification pipeline using multiple signals.
     *
     * Priority order:
     * 1. Emergency squawk (7500/7600/7700)
     * 2. Emergency callsigns (LIFEGUARD/MEDEVAC)
     * 3. Military/Government classifier (ICAO hex, callsign, type code)
     * 4. Cargo airline prefixes
     * 5. ADS-B category A7 → HELICOPTER
     * 6. ADS-B category B* → GROUND_VEHICLE
     * 7. Standard A1-A6 mapping
     * 8. Fallback → UNKNOWN
     */
    private fun classifyAircraft(
        hex: String?,
        callsign: String?,
        typeCode: String?,
        registration: String?,
        adsbCategory: String?,
        squawk: String?
    ): Pair<ObjectCategory, List<String>> {
        val signals = mutableListOf<String>()

        // 1. Emergency squawk overrides everything
        if (squawk != null && squawk in setOf("7500", "7600", "7700")) {
            signals.add("SQUAWK:$squawk")
            return ObjectCategory.EMERGENCY to signals
        }

        // 2. Emergency callsigns
        if (callsign != null && emergencyCallsignPattern.matches(callsign)) {
            signals.add("CALLSIGN:EMERGENCY")
            return ObjectCategory.EMERGENCY to signals
        }

        // 3. Military/Government classifier
        val milResult = MilitaryClassifier.classify(hex, callsign, typeCode, registration)
        if (milResult.category != null) {
            signals.addAll(milResult.signals)
            return milResult.category to signals
        }

        // 4. Cargo airline prefixes
        if (callsign != null && callsign.length >= 3) {
            val prefix = callsign.take(3).uppercase()
            if (prefix in cargoCallsignPrefixes) {
                signals.add("CALLSIGN:CARGO_$prefix")
                return ObjectCategory.CARGO to signals
            }
        }

        // 5-8. ADS-B category-based classification
        val baseCategory = mapAdsbxCategory(adsbCategory)
        return baseCategory to signals
    }

    /**
     * Map ADSBx category string (e.g. "A1"-"A7") to domain ObjectCategory.
     */
    private fun mapAdsbxCategory(category: String?): ObjectCategory {
        return when {
            category == null -> ObjectCategory.UNKNOWN
            category.startsWith("A") -> {
                val num = category.removePrefix("A").toIntOrNull() ?: 0
                when (num) {
                    1, 2 -> ObjectCategory.GENERAL_AVIATION  // Light / Small
                    3, 4, 5, 6 -> ObjectCategory.COMMERCIAL  // Large / High vortex / Heavy / High perf
                    7 -> ObjectCategory.HELICOPTER            // Rotorcraft
                    else -> ObjectCategory.UNKNOWN
                }
            }
            category.startsWith("B") -> ObjectCategory.GROUND_VEHICLE
            else -> ObjectCategory.UNKNOWN
        }
    }

    /**
     * Handle HTTP exceptions, detecting 429 rate limiting.
     */
    private fun handleHttpException(e: HttpException): FetchException {
        if (e.code() == 429) {
            val retryAfter = e.response()?.headers()?.get("Retry-After")?.toIntOrNull() ?: 30
            return FetchException.RateLimited(retryAfter)
        }
        return FetchException.HttpError(e.code(), e.message() ?: "Unknown HTTP error")
    }

    /**
     * Parse an OpenSky state vector array into an Aircraft domain object.
     */
    private fun parseStateVector(state: List<Any?>): Aircraft? {
        if (state.size < 17) return null

        val icao = state[0] as? String ?: return null
        val callsign = (state[1] as? String)?.trim()?.ifBlank { null }
        val lastContact = (state[4] as? Double)?.toLong() ?: return null
        val longitude = state[5] as? Double ?: return null
        val latitude = state[6] as? Double ?: return null
        val altitude = state[7] as? Double ?: state[13] as? Double ?: 0.0
        val onGround = state[8] as? Boolean ?: false
        val velocity = (state[9] as? Double)?.toFloat()
        val heading = (state[10] as? Double)?.toFloat()
        val squawk = state[14] as? String
        val categoryInt = if (state.size > 17) (state[17] as? Double)?.toInt() else null

        // Apply classification pipeline (OpenSky has no type code or ADS-B category string)
        val openSkyBase = mapCategory(categoryInt)
        val (classifiedCategory, signals) = classifyAircraft(
            hex = icao,
            callsign = callsign,
            typeCode = null,
            registration = null,
            adsbCategory = null,
            squawk = squawk
        )
        // Use classified result if it found something specific, otherwise use OpenSky's category
        val finalCategory = if (classifiedCategory != ObjectCategory.UNKNOWN) classifiedCategory else openSkyBase

        return Aircraft(
            id = icao,
            position = Position(
                latitude = latitude,
                longitude = longitude,
                altitudeMeters = altitude,
                heading = heading,
                speedMps = velocity
            ),
            category = finalCategory,
            firstSeen = Instant.now(),
            lastUpdated = Instant.ofEpochSecond(lastContact),
            icaoHex = icao,
            callsign = callsign,
            squawk = squawk,
            isOnGround = onGround,
            classificationSignals = signals.ifEmpty { null }
        )
    }

    /**
     * Map OpenSky integer category to domain ObjectCategory.
     */
    private fun mapCategory(category: Int?): ObjectCategory {
        return when (category) {
            2 -> ObjectCategory.GENERAL_AVIATION
            3, 4, 5, 6 -> ObjectCategory.COMMERCIAL
            7 -> ObjectCategory.MILITARY
            8 -> ObjectCategory.HELICOPTER
            else -> ObjectCategory.UNKNOWN
        }
    }
}
