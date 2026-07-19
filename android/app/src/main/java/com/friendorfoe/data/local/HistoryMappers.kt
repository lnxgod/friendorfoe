package com.friendorfoe.data.local

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import java.time.Instant

/** Convert a live domain object into the single persisted history contract. */
internal fun SkyObject.toHistoryEntity(
    userLatitude: Double,
    userLongitude: Double,
): HistoryEntity = when (this) {
    is Aircraft -> HistoryEntity(
        objectId = icaoHex,
        objectType = "aircraft",
        detectionSource = source.name.lowercase(),
        category = category.name.lowercase(),
        displayName = callsign ?: icaoHex,
        description = displaySummary(),
        latitude = position.latitude,
        longitude = position.longitude,
        altitudeMeters = position.altitudeMeters,
        userLatitude = userLatitude,
        userLongitude = userLongitude,
        distanceMeters = distanceMeters,
        confidence = confidence,
        firstSeen = firstSeen.toEpochMilli(),
        lastSeen = lastUpdated.toEpochMilli(),
        photoUrl = photoUrl,
    )

    is Drone -> HistoryEntity(
        objectId = id,
        objectType = "drone",
        detectionSource = source.name.lowercase(),
        category = category.name.lowercase(),
        displayName = manufacturer ?: "Unknown drone",
        description = displaySummary(),
        latitude = position.latitude,
        longitude = position.longitude,
        altitudeMeters = position.altitudeMeters,
        userLatitude = userLatitude,
        userLongitude = userLongitude,
        distanceMeters = distanceMeters,
        confidence = confidence,
        firstSeen = firstSeen.toEpochMilli(),
        lastSeen = lastUpdated.toEpochMilli(),
        ssid = ssid,
        bssid = bssid,
        signalStrengthDbm = signalStrengthDbm,
        frequencyMhz = frequencyMhz,
        channelWidthMhz = channelWidthMhz,
    )
}

/** Rebuild a historical drone without inventing radio evidence for old rows. */
internal fun HistoryEntity.toDrone(): Drone = Drone(
    id = objectId,
    position = Position(
        latitude = latitude,
        longitude = longitude,
        altitudeMeters = altitudeMeters,
    ),
    source = enumValueOrDefault(detectionSource, DetectionSource.ADS_B),
    category = enumValueOrDefault(category, ObjectCategory.UNKNOWN),
    confidence = confidence,
    firstSeen = Instant.ofEpochMilli(firstSeen),
    lastUpdated = Instant.ofEpochMilli(lastSeen),
    distanceMeters = distanceMeters,
    droneId = objectId,
    manufacturer = displayName,
    ssid = ssid,
    bssid = bssid,
    signalStrengthDbm = signalStrengthDbm,
    frequencyMhz = frequencyMhz,
    channelWidthMhz = channelWidthMhz,
)

private inline fun <reified T : Enum<T>> enumValueOrDefault(raw: String, fallback: T): T =
    runCatching { enumValueOf<T>(raw.uppercase()) }.getOrDefault(fallback)
