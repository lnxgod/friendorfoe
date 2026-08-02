package com.friendorfoe.presentation.detail

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt

data class DetailField(
    val label: String,
    val value: String,
)

data class DetailIdentifier(
    val label: String,
    val value: String,
    val copyable: Boolean,
)

data class AircraftVisual(
    val photoUrl: String?,
    val typeCode: String?,
    val description: String?,
    val category: ObjectCategory,
)

data class DetailPresentation(
    val title: String,
    val statusLabel: String,
    val isLive: Boolean,
    val summary: List<DetailField>,
    val identifiers: List<DetailIdentifier>,
    val advanced: List<DetailField>,
    val raw: List<DetailField>,
    val retryLabel: String?,
    val supportingMessage: String? = null,
    val rawExpandedByDefault: Boolean = false,
    val aircraftVisual: AircraftVisual? = null,
)

fun presentHistoricalDetail(row: HistoryEntity): DetailPresentation {
    val category = historicalCategory(row.category)
    return DetailPresentation(
        title = row.displayName.takeIf(String::isNotBlank) ?: row.objectId,
        statusLabel = "Historical detection",
        isLive = false,
        summary = listOf(
            DetailField("Source", historicalSourceLabel(row.detectionSource)),
            DetailField("Category", historyCategoryLabel(row.category)),
            DetailField("Observed", formatDetailInstant(Instant.ofEpochMilli(row.lastSeen))),
        ),
        identifiers = listOfNotNull(
            identifier("Object ID", row.objectId),
        ),
        advanced = buildList {
            row.description?.takeIf(String::isNotBlank)?.let { add(DetailField("Description", it)) }
            row.distanceMeters?.let { add(DetailField("Distance at observation", formatDetailDistance(it))) }
            formatKnownPosition(row.latitude, row.longitude)?.let {
                add(DetailField("Position at observation", it))
            }
            add(DetailField("Altitude at observation", formatAltitude(row.altitudeMeters)))
            add(DetailField("Confidence", formatConfidence(row.confidence)))
        },
        raw = listOf(
            DetailField("History record", row.id.toString()),
            DetailField("Object type", row.objectType),
            DetailField("First observed", formatDetailInstant(Instant.ofEpochMilli(row.firstSeen))),
            DetailField("Last observed", formatDetailInstant(Instant.ofEpochMilli(row.lastSeen))),
        ) + listOfNotNull(
            formatKnownPosition(row.userLatitude, row.userLongitude)?.let {
                DetailField("Phone position at observation", it)
            },
        ),
        retryLabel = null,
        aircraftVisual = if (row.objectType.equals("aircraft", ignoreCase = true)) {
            AircraftVisual(
                photoUrl = row.photoUrl?.takeIf(String::isNotBlank),
                typeCode = null,
                description = null,
                category = category,
            )
        } else {
            null
        },
    )
}

fun presentLiveDetail(
    aircraft: Aircraft,
    remoteDetail: AircraftDetailDto?,
    remoteFailure: String?,
): DetailPresentation {
    val callsign = firstNonBlank(remoteDetail?.callsign, aircraft.callsign)
    val registration = firstNonBlank(remoteDetail?.registration, aircraft.registration)
    val aircraftType = firstNonBlank(remoteDetail?.aircraftType, aircraft.aircraftType)
    val model = firstNonBlank(remoteDetail?.aircraftDescription, aircraft.aircraftModel)
    val operator = firstNonBlank(remoteDetail?.operator, aircraft.operatorName, aircraft.airline)
    val origin = firstNonBlank(remoteDetail?.route?.origin, aircraft.origin)
    val destination = firstNonBlank(remoteDetail?.route?.destination, aircraft.destination)

    return DetailPresentation(
        title = firstNonBlank(callsign, registration, aircraft.icaoHex, "Aircraft")!!,
        statusLabel = "Live detection",
        isLive = true,
        summary = buildList {
            add(DetailField("Source", aircraft.source.humanLabel()))
            add(DetailField("Category", aircraft.category.humanLabel()))
            add(DetailField("Last observed", formatDetailInstant(aircraft.lastUpdated)))
            aircraft.distanceMeters?.let { add(DetailField("Distance", formatDetailDistance(it))) }
        },
        identifiers = listOfNotNull(
            identifier("ICAO address", aircraft.icaoHex),
            identifier("Callsign", callsign),
            identifier("Registration", registration),
        ),
        advanced = buildList {
            aircraftType?.let { add(DetailField("Aircraft type", it)) }
            model?.let { add(DetailField("Model", it)) }
            operator?.let { add(DetailField("Operator", it)) }
            remoteDetail?.country?.takeIf(String::isNotBlank)?.let { add(DetailField("Country", it)) }
            routeLabel(origin, destination)?.let { add(DetailField("Route", it)) }
            aircraft.position.formatKnown()?.let { add(DetailField("Position", it)) }
            add(DetailField("Altitude", formatAltitude(aircraft.position.altitudeMeters)))
            aircraft.position.speedMps?.let { add(DetailField("Speed", formatSpeed(it))) }
            aircraft.position.heading?.let { add(DetailField("Heading", "${it.roundToInt()}°")) }
            add(DetailField("Confidence", formatConfidence(aircraft.confidence)))
        },
        raw = buildList {
            add(DetailField("First observed", formatDetailInstant(aircraft.firstSeen)))
            aircraft.squawk?.takeIf(String::isNotBlank)?.let { add(DetailField("Squawk", it)) }
            add(DetailField("On ground", if (aircraft.isOnGround) "Yes" else "No"))
        },
        retryLabel = remoteFailure?.takeIf(String::isNotBlank)?.let { "Retry details" },
        supportingMessage = remoteFailure?.takeIf(String::isNotBlank),
        aircraftVisual = AircraftVisual(
            photoUrl = aircraft.photoUrl,
            typeCode = aircraftType,
            description = model,
            category = aircraft.category,
        ),
    )
}

fun presentLiveDroneDetail(drone: Drone): DetailPresentation = DetailPresentation(
    title = listOfNotNull(
        drone.manufacturer?.takeIf(String::isNotBlank),
        drone.model?.takeIf(String::isNotBlank),
    ).joinToString(" ").ifBlank { "Drone" },
    statusLabel = "Live detection",
    isLive = true,
    summary = buildList {
        add(DetailField("Source", drone.source.humanLabel()))
        add(DetailField("Category", drone.category.humanLabel()))
        add(DetailField("Last observed", formatDetailInstant(drone.lastUpdated)))
        drone.distanceMeters?.let { add(DetailField("Distance", formatDetailDistance(it))) }
    },
    identifiers = listOfNotNull(
        identifier("Broadcast ID", drone.droneId),
        identifier("Operator ID", drone.operatorId),
        identifier("Network name", drone.ssid),
        identifier("Transmitter address", drone.bssid),
    ),
    advanced = buildList {
        drone.manufacturer?.takeIf(String::isNotBlank)?.let { add(DetailField("Manufacturer", it)) }
        drone.model?.takeIf(String::isNotBlank)?.let { add(DetailField("Model", it)) }
        drone.uaTypeLabel()?.takeIf(String::isNotBlank)?.let { add(DetailField("Aircraft type", it)) }
        drone.idTypeLabel()?.takeIf(String::isNotBlank)?.let { add(DetailField("ID type", it)) }
        drone.signalStrengthDbm?.let { add(DetailField("Signal", "$it dBm")) }
        drone.estimatedDistanceMeters?.let {
            add(DetailField("Estimated signal distance", formatDetailDistance(it)))
        }
        drone.position.formatKnown()?.let { add(DetailField("Position", it)) }
        add(DetailField("Altitude", formatAltitude(drone.position.altitudeMeters)))
        operatorPosition(drone)?.let { add(DetailField("Operator position", it)) }
        add(DetailField("Confidence", formatConfidence(drone.confidence)))
    },
    raw = buildList {
        add(DetailField("First observed", formatDetailInstant(drone.firstSeen)))
        drone.frequencyMhz?.let { add(DetailField("Frequency", "$it MHz")) }
        drone.channelWidthMhz?.let { add(DetailField("Channel width", "$it MHz")) }
        drone.selfIdText?.takeIf(String::isNotBlank)?.let { add(DetailField("Self ID", it)) }
    },
    retryLabel = null,
)

private fun identifier(label: String, value: String?): DetailIdentifier? = value
    ?.takeIf(String::isNotBlank)
    ?.let { DetailIdentifier(label = label, value = it, copyable = true) }

private fun firstNonBlank(vararg values: String?): String? =
    values.firstOrNull { !it.isNullOrBlank() }

private fun DetectionSource.humanLabel(): String = when (this) {
    DetectionSource.ADS_B -> "ADS-B"
    DetectionSource.REMOTE_ID -> "Remote ID"
    DetectionSource.WIFI_NAN,
    DetectionSource.WIFI_BEACON,
    -> "Remote ID · Wi-Fi"
    DetectionSource.WIFI -> "Phone"
}

private fun historicalSourceLabel(source: String): String = when (source.trim().lowercase()) {
    "ads_b" -> "ADS-B"
    "remote_id" -> "Remote ID"
    "wifi_nan", "wifi_beacon" -> "Remote ID · Wi-Fi"
    "wifi" -> "Phone"
    "configured_backend" -> "Configured backend"
    else -> source.trim().ifBlank { "Unspecified source" }
}

private fun ObjectCategory.humanLabel(): String = when (this) {
    ObjectCategory.COMMERCIAL -> "Commercial"
    ObjectCategory.GENERAL_AVIATION -> "General aviation"
    ObjectCategory.MILITARY -> "Military"
    ObjectCategory.HELICOPTER -> "Helicopter"
    ObjectCategory.GOVERNMENT -> "Government"
    ObjectCategory.EMERGENCY -> "Emergency"
    ObjectCategory.CARGO -> "Cargo"
    ObjectCategory.DRONE -> "Drone / UAS"
    ObjectCategory.GROUND_VEHICLE -> "Ground vehicle"
    ObjectCategory.UNKNOWN -> "Unclassified"
}

private fun historyCategoryLabel(category: String): String = ObjectCategory.entries
    .firstOrNull { it.name.equals(category.trim(), ignoreCase = true) }
    ?.humanLabel()
    ?: category.replace('_', ' ').trim().replaceFirstChar { it.titlecase(Locale.US) }
        .ifBlank { "Unclassified" }

private fun historicalCategory(raw: String): ObjectCategory = ObjectCategory.entries
    .firstOrNull { it.name.equals(raw.trim(), ignoreCase = true) }
    ?: ObjectCategory.UNKNOWN

private fun Position.formatKnown(): String? = formatKnownPosition(latitude, longitude)

private fun formatKnownPosition(latitude: Double, longitude: Double): String? =
    if (latitude == 0.0 && longitude == 0.0) null else formatPosition(latitude, longitude)

private fun formatPosition(latitude: Double, longitude: Double): String =
    String.format(Locale.US, "%.5f, %.5f", latitude, longitude)

private fun operatorPosition(drone: Drone): String? {
    val latitude = drone.operatorLatitude ?: return null
    val longitude = drone.operatorLongitude ?: return null
    return formatKnownPosition(latitude, longitude)
}

private fun formatAltitude(meters: Double): String =
    "${(meters * METERS_TO_FEET).roundToInt()} ft"

private fun formatSpeed(metersPerSecond: Float): String =
    "${(metersPerSecond * METERS_PER_SECOND_TO_KNOTS).roundToInt()} kt"

private fun formatConfidence(confidence: Float): String =
    "${(confidence.coerceIn(0f, 1f) * 100f).roundToInt()}%"

private fun formatDetailDistance(meters: Double): String = if (meters >= 800.0) {
    val miles = meters / METERS_PER_MILE
    if (miles >= 10.0) String.format(Locale.US, "%.0f mi", miles)
    else String.format(Locale.US, "%.1f mi", miles)
} else {
    "${meters.roundToInt()} m"
}

private fun routeLabel(origin: String?, destination: String?): String? = when {
    origin != null && destination != null -> "$origin → $destination"
    origin != null -> "From $origin"
    destination != null -> "To $destination"
    else -> null
}

private fun formatDetailInstant(value: Instant): String = DETAIL_TIME_FORMAT.format(value)

private val DETAIL_TIME_FORMAT: DateTimeFormatter = DateTimeFormatter
    .ofPattern("MMM d, yyyy · h:mm a", Locale.US)
    .withZone(ZoneId.systemDefault())

private const val METERS_TO_FEET = 3.28084
private const val METERS_PER_SECOND_TO_KNOTS = 1.94384
private const val METERS_PER_MILE = 1_609.344
