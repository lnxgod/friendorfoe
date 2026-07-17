package com.friendorfoe.presentation.map

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.data.remote.SensorDto
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.util.categoryColorArgb
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polygon
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.roundToInt
import kotlin.math.sin

internal fun mapTrackAlpha(confidence: Float): Float {
    val bounded = confidence.coerceIn(0f, 1f)
    return if (bounded <= 0.5f) {
        0.25f + (bounded * 0.7f)
    } else {
        0.6f + ((bounded - 0.5f) * 0.8f)
    }
}

internal data class OsmdroidMarkerOrientation(
    val rotationDegrees: Float,
    val flat: Boolean = true,
)

/** Converts clockwise compass headings to osmdroid's counter-clockwise bearing. */
internal fun osmdroidMarkerOrientation(headingDegrees: Float?): OsmdroidMarkerOrientation {
    if (headingDegrees == null || !headingDegrees.isFinite()) {
        return OsmdroidMarkerOrientation(rotationDegrees = 0f)
    }

    val normalized = ((headingDegrees % 360f) + 360f) % 360f
    var rotation = -normalized
    if (rotation < -180f) rotation += 360f
    if (rotation == 0f) rotation = 0f
    return OsmdroidMarkerOrientation(rotationDegrees = rotation)
}

private fun Marker.setMapHeading(headingDegrees: Float?) {
    val orientation = osmdroidMarkerOrientation(headingDegrees)
    setFlat(orientation.flat)
    setRotation(orientation.rotationDegrees)
}

internal data class AircraftMarkerPresentation(
    val key: String,
    val latitude: Double,
    val longitude: Double,
    val rotationDegrees: Float,
    val alpha: Float,
    val title: String,
    val snippet: String,
    val category: ObjectCategory,
    val visuallyConfirmed: Boolean,
)

internal fun MapTrack.toAircraftMarkerPresentation(
    visuallyConfirmed: Boolean,
): AircraftMarkerPresentation = AircraftMarkerPresentation(
    key = skyObject.id,
    latitude = position.latitude,
    longitude = position.longitude,
    rotationDegrees = headingDegrees ?: 0f,
    alpha = mapTrackAlpha(confidence),
    title = markerTitle(skyObject),
    snippet = markerSnippet(skyObject),
    category = skyObject.category,
    visuallyConfirmed = visuallyConfirmed,
)

internal class RetainedOverlayStore<T> {
    private val retained = linkedMapOf<String, T>()

    val keys: Set<String>
        get() = retained.keys.toSet()

    operator fun get(key: String): T? = retained[key]

    fun <S> render(
        desired: List<S>,
        keyOf: (S) -> String,
        create: (S) -> T,
        update: (T, S) -> Unit,
        remove: (T) -> Unit,
    ) {
        val desiredKeys = desired.mapTo(mutableSetOf(), keyOf)
        val departedKeys = retained.keys.filterNot(desiredKeys::contains)
        departedKeys.forEach { key -> retained.remove(key)?.let(remove) }

        desired.forEach { state ->
            val key = keyOf(state)
            val overlay = retained[key] ?: create(state).also { retained[key] = it }
            update(overlay, state)
        }
    }
}

internal data class DistanceRingKey(
    val latitude: Double,
    val longitude: Double,
    val zoomBucket: Int,
    val remoteLatitude: Double?,
    val remoteLongitude: Double?,
)

internal fun distanceRingKey(
    center: Position,
    zoomLevel: Double,
    remoteCenter: Position?,
): DistanceRingKey = DistanceRingKey(
    latitude = center.latitude,
    longitude = center.longitude,
    zoomBucket = floor(zoomLevel).toInt(),
    remoteLatitude = remoteCenter?.latitude,
    remoteLongitude = remoteCenter?.longitude,
)

internal enum class SensorOverlayKind {
    RANGE,
    ACCURACY,
    MARKER,
}

internal data class SensorOverlayKey(
    val objectId: String,
    val kind: SensorOverlayKind,
)

internal fun sensorOverlayDrawOrder(drones: List<LocatedDroneDto>): List<SensorOverlayKey> = buildList {
    drones.forEach { drone ->
        if (drone.positionSource == "range_only" && drone.rangeM != null) {
            add(SensorOverlayKey(drone.droneId, SensorOverlayKind.RANGE))
        }
    }
    drones.forEach { drone ->
        if (drone.positionSource == "trilateration" &&
            drone.accuracyM != null && drone.accuracyM > 10
        ) {
            add(SensorOverlayKey(drone.droneId, SensorOverlayKind.ACCURACY))
        }
    }
    drones.forEach { drone ->
        add(SensorOverlayKey(drone.droneId, SensorOverlayKind.MARKER))
    }
}

internal class MapOverlayController(
    private val context: Context,
    private val map: MapView,
    private val onLocalObjectSelected: (String) -> Unit,
    private val onRemoteObjectSelected: (String) -> Unit,
) {
    private data class AircraftIconKey(
        val category: ObjectCategory,
        val visuallyConfirmed: Boolean,
    )

    private data class AircraftOverlay(
        val marker: Marker,
        var iconKey: AircraftIconKey? = null,
    )

    private data class RemoteOverlay(
        val marker: Marker,
        var category: ObjectCategory? = null,
    )

    private data class SensorDroneOverlay(
        val marker: Marker,
        var color: Int? = null,
    )

    private data class SensorDroneShape(
        val key: String,
        val center: GeoPoint,
        val radiusMeters: Double,
        val title: String?,
    )

    private val aircraftMarkers = RetainedOverlayStore<AircraftOverlay>()
    private val remoteMarkers = RetainedOverlayStore<RemoteOverlay>()
    private val sensorMarkers = RetainedOverlayStore<Marker>()
    private val sensorDroneMarkers = RetainedOverlayStore<SensorDroneOverlay>()
    private val sensorRangeShapes = RetainedOverlayStore<Polygon>()
    private val sensorAccuracyShapes = RetainedOverlayStore<Polygon>()

    private var userMarker: Marker? = null
    private var userMarkerFollowsCompass: Boolean? = null
    private var fovCone: Polygon? = null
    private var remoteSearchMarker: Marker? = null
    private var distanceRingKey: DistanceRingKey? = null
    private val distanceRings = mutableListOf<Polygon>()

    fun render(
        mapTracks: List<MapTrack>,
        userPosition: Position,
        followCompass: Boolean,
        stabilizedMapHeading: Float,
        activeVisualFocusIds: Set<String>,
        remoteSensors: List<SensorDto>,
        sensorDrones: List<LocatedDroneDto>,
        remoteSearchResults: List<SkyObject>,
        remoteSearchCenter: Position?,
        isUserPanning: Boolean,
    ) {
        map.mapOrientation = if (followCompass) -stabilizedMapHeading else 0f

        if (userPosition.hasMapCoordinates()) {
            val userGeoPoint = userPosition.toGeoPoint()
            updateDistanceRings(userPosition, remoteSearchCenter)
            updateRemoteSearchMarker(remoteSearchCenter, remoteSearchResults.size)
            updateFovCone(userGeoPoint, followCompass, stabilizedMapHeading)
            updateUserMarker(userGeoPoint, followCompass, stabilizedMapHeading)
            updateAircraftMarkers(mapTracks, activeVisualFocusIds)
            updateSensorMarkers(remoteSensors)
            updateSensorDroneOverlays(sensorDrones)
            updateRemoteMarkers(remoteSearchResults, mapTracks.mapTo(mutableSetOf()) { it.skyObject.id })
            updateCenter(userGeoPoint, followCompass, isUserPanning)
        }

        map.invalidate()
    }

    private fun updateAircraftMarkers(
        tracks: List<MapTrack>,
        activeVisualFocusIds: Set<String>,
    ) {
        val presentations = tracks
            .filter { it.position.hasMapCoordinates() }
            .map { track ->
                track.toAircraftMarkerPresentation(track.skyObject.id in activeVisualFocusIds)
            }

        aircraftMarkers.render(
            desired = presentations,
            keyOf = AircraftMarkerPresentation::key,
            create = { state ->
                val rawObjectId = state.key
                AircraftOverlay(
                    Marker(map).apply {
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        setOnMarkerClickListener { _, _ ->
                            onLocalObjectSelected(rawObjectId)
                            true
                        }
                        map.overlays.add(this)
                    }
                )
            },
            update = { overlay, state ->
                overlay.marker.setPosition(GeoPoint(state.latitude, state.longitude))
                overlay.marker.setMapHeading(state.rotationDegrees)
                overlay.marker.setAlpha(state.alpha)
                overlay.marker.title = state.title
                overlay.marker.snippet = state.snippet

                val nextIconKey = AircraftIconKey(state.category, state.visuallyConfirmed)
                if (overlay.iconKey != nextIconKey) {
                    overlay.marker.icon = createCategoryMarkerDrawable(
                        context = context,
                        category = state.category,
                        color = categoryColorArgb(state.category),
                        heading = 0f,
                        visuallyConfirmed = state.visuallyConfirmed,
                    )
                    overlay.iconKey = nextIconKey
                }
            },
            remove = { it.marker.remove(map) },
        )
    }

    private fun updateRemoteMarkers(
        remoteSearchResults: List<SkyObject>,
        localObjectIds: Set<String>,
    ) {
        val desired = remoteSearchResults.filter {
            it.position.hasMapCoordinates() && it.id !in localObjectIds
        }
        remoteMarkers.render(
            desired = desired,
            keyOf = SkyObject::id,
            create = { obj ->
                val rawObjectId = obj.id
                RemoteOverlay(
                    Marker(map).apply {
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        setOnMarkerClickListener { _, _ ->
                            onRemoteObjectSelected(rawObjectId)
                            true
                        }
                        map.overlays.add(this)
                    }
                )
            },
            update = { overlay, obj ->
                overlay.marker.setPosition(obj.position.toGeoPoint())
                overlay.marker.setMapHeading(obj.position.heading)
                overlay.marker.setAlpha(1f)
                overlay.marker.title = markerTitle(obj)
                overlay.marker.snippet = "Remote: ${markerSnippet(obj)}"
                if (overlay.category != obj.category) {
                    overlay.marker.icon = createCategoryMarkerDrawable(
                        context,
                        obj.category,
                        0xFF00BCD4.toInt(),
                        0f,
                    )
                    overlay.category = obj.category
                }
            },
            remove = { it.marker.remove(map) },
        )
    }

    private fun updateSensorMarkers(sensors: List<SensorDto>) {
        sensorMarkers.render(
            desired = sensors,
            keyOf = SensorDto::deviceId,
            create = {
                Marker(map).apply {
                    setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                    icon = createSensorDrawable(context)
                    map.overlays.add(this)
                }
            },
            update = { marker, sensor ->
                marker.setPosition(GeoPoint(sensor.lat, sensor.lon))
                marker.setRotation(0f)
                marker.setAlpha(1f)
                marker.title = "Sensor: ${sensor.deviceId}"
                marker.snippet = null
            },
            remove = { it.remove(map) },
        )
    }

    private fun updateSensorDroneOverlays(drones: List<LocatedDroneDto>) {
        val rangeShapes = drones.mapNotNull { drone ->
            val radius = drone.rangeM
            if (drone.positionSource != "range_only" || radius == null) return@mapNotNull null
            SensorDroneShape(
                key = drone.droneId,
                center = GeoPoint(drone.lat, drone.lon),
                radiusMeters = radius,
                title = "Range: ~${radius.toInt()}m",
            )
        }
        sensorRangeShapes.render(
            desired = rangeShapes,
            keyOf = SensorDroneShape::key,
            create = { shape -> createSensorRangeShape(shape).also(map.overlays::add) },
            update = { polygon, shape ->
                polygon.points = Polygon.pointsAsCircle(shape.center, shape.radiusMeters)
                polygon.title = shape.title
            },
            remove = map.overlays::remove,
        )

        val accuracyShapes = drones.mapNotNull { drone ->
            val radius = drone.accuracyM
            if (drone.positionSource != "trilateration" || radius == null || radius <= 10) {
                return@mapNotNull null
            }
            SensorDroneShape(
                key = drone.droneId,
                center = GeoPoint(drone.lat, drone.lon),
                radiusMeters = radius,
                title = null,
            )
        }
        sensorAccuracyShapes.render(
            desired = accuracyShapes,
            keyOf = SensorDroneShape::key,
            create = { shape -> createSensorAccuracyShape(shape).also(map.overlays::add) },
            update = { polygon, shape ->
                polygon.points = Polygon.pointsAsCircle(shape.center, shape.radiusMeters)
            },
            remove = map.overlays::remove,
        )

        sensorDroneMarkers.render(
            desired = drones,
            keyOf = LocatedDroneDto::droneId,
            create = {
                SensorDroneOverlay(
                    Marker(map).apply {
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        map.overlays.add(this)
                    }
                )
            },
            update = { overlay, drone ->
                overlay.marker.setPosition(GeoPoint(drone.lat, drone.lon))
                overlay.marker.setMapHeading(drone.headingDeg)
                overlay.marker.setAlpha(1f)
                overlay.marker.title = sensorDroneTitle(drone)
                overlay.marker.snippet = sensorDroneSnippet(drone)
                val nextColor = sensorDroneColor(drone)
                if (overlay.color != nextColor) {
                    overlay.marker.icon = createSensorDroneDrawable(context, nextColor)
                    overlay.color = nextColor
                }
            },
            remove = { it.marker.remove(map) },
        )

        enforceSensorOverlayDrawOrder(drones)
    }

    private fun enforceSensorOverlayDrawOrder(drones: List<LocatedDroneDto>) {
        val orderedOverlays = sensorOverlayDrawOrder(drones).mapNotNull { key ->
            when (key.kind) {
                SensorOverlayKind.RANGE -> sensorRangeShapes[key.objectId]
                SensorOverlayKind.ACCURACY -> sensorAccuracyShapes[key.objectId]
                SensorOverlayKind.MARKER -> sensorDroneMarkers[key.objectId]?.marker
            }
        }
        if (orderedOverlays.isEmpty()) return

        val managedOverlays = orderedOverlays.toSet()
        val currentOrder = map.overlays.filter(managedOverlays::contains)
        if (currentOrder == orderedOverlays) return

        val insertionIndex = map.overlays.indexOfFirst(managedOverlays::contains)
            .takeIf { it >= 0 } ?: map.overlays.size
        map.overlays.removeAll(managedOverlays)
        map.overlays.addAll(insertionIndex.coerceAtMost(map.overlays.size), orderedOverlays)
    }

    private fun updateDistanceRings(userPosition: Position, remoteCenter: Position?) {
        val nextKey = distanceRingKey(userPosition, map.zoomLevelDouble, remoteCenter)
        if (nextKey == distanceRingKey) return

        distanceRings.forEach(map.overlays::remove)
        distanceRings.clear()
        val userCenter = userPosition.toGeoPoint()
        distanceRings += createDistanceRing(userCenter, 10.0).also(map.overlays::add)
        distanceRings += createDistanceRing(userCenter, 25.0).also(map.overlays::add)
        remoteCenter?.let {
            distanceRings += createDistanceRing(it.toGeoPoint(), 250.0).also(map.overlays::add)
        }
        distanceRingKey = nextKey
    }

    private fun updateRemoteSearchMarker(remoteCenter: Position?, resultCount: Int) {
        if (remoteCenter == null) {
            remoteSearchMarker?.remove(map)
            remoteSearchMarker = null
            return
        }

        val marker = remoteSearchMarker ?: Marker(map).apply {
            setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_BOTTOM)
            map.overlays.add(this)
        }.also { remoteSearchMarker = it }
        marker.setPosition(remoteCenter.toGeoPoint())
        marker.setRotation(0f)
        marker.setAlpha(1f)
        marker.title = "Search area (250 NM)"
        marker.snippet = "$resultCount aircraft found"
    }

    private fun updateFovCone(center: GeoPoint, following: Boolean, headingDegrees: Float) {
        if (!following) {
            fovCone?.let(map.overlays::remove)
            fovCone = null
            return
        }

        val cone = fovCone ?: Polygon(map).apply {
            fillPaint.apply {
                color = 0x302196F3
                style = Paint.Style.FILL
            }
            outlinePaint.apply {
                color = 0x802196F3.toInt()
                strokeWidth = 2f
                style = Paint.Style.STROKE
            }
            map.overlays.add(this)
        }.also { fovCone = it }
        cone.points = fovConePoints(center, headingDegrees, map.zoomLevelDouble)
    }

    private fun updateUserMarker(center: GeoPoint, following: Boolean, headingDegrees: Float) {
        val marker = userMarker ?: Marker(map).apply {
            setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
            title = "You"
            map.overlays.add(this)
        }.also { userMarker = it }

        marker.setPosition(center)
        marker.setMapHeading(if (following) headingDegrees else null)
        marker.setAlpha(1f)
        marker.title = "You"
        marker.snippet = null
        if (userMarkerFollowsCompass != following) {
            marker.icon = createUserDrawable(context, following)
            userMarkerFollowsCompass = following
        }
    }

    private fun updateCenter(userGeoPoint: GeoPoint, following: Boolean, isUserPanning: Boolean) {
        if (isUserPanning) return
        if (following) {
            map.controller.animateTo(userGeoPoint)
            return
        }

        val currentCenter = map.mapCenter
        val distanceMeters = userGeoPoint.distanceToAsDouble(
            GeoPoint(currentCenter.latitude, currentCenter.longitude)
        )
        if (distanceMeters > 500) map.controller.animateTo(userGeoPoint)
    }

    private fun createDistanceRing(center: GeoPoint, radiusNm: Double): Polygon = Polygon(map).apply {
        points = Polygon.pointsAsCircle(center, radiusNm * 1852.0)
        fillPaint.apply {
            color = 0x10FFFFFF
            style = Paint.Style.FILL
        }
        outlinePaint.apply {
            color = 0x80FFFFFF.toInt()
            strokeWidth = 2f
            style = Paint.Style.STROKE
        }
        title = "${radiusNm.toInt()} NM"
    }

    private fun createSensorRangeShape(shape: SensorDroneShape): Polygon = Polygon(map).apply {
        points = Polygon.pointsAsCircle(shape.center, shape.radiusMeters)
        fillPaint.apply {
            color = 0x20FF6D00
            style = Paint.Style.FILL
        }
        outlinePaint.apply {
            color = 0xAAFF6D00.toInt()
            strokeWidth = 2f
            style = Paint.Style.STROKE
        }
        title = shape.title
    }

    private fun createSensorAccuracyShape(shape: SensorDroneShape): Polygon = Polygon(map).apply {
        points = Polygon.pointsAsCircle(shape.center, shape.radiusMeters)
        fillPaint.apply {
            color = 0x15E91E63
            style = Paint.Style.FILL
        }
        outlinePaint.apply {
            color = 0xAAE91E63.toInt()
            strokeWidth = 1.5f
            style = Paint.Style.STROKE
        }
    }
}

private fun markerTitle(obj: SkyObject): String = when (obj) {
    is Aircraft -> {
        val callsign = obj.callsign ?: obj.icaoHex
        if (obj.aircraftType != null) "$callsign (${obj.aircraftType})" else callsign
    }
    is Drone -> obj.droneId
}

private fun markerSnippet(obj: SkyObject): String = when (obj) {
    is Aircraft -> {
        val altitudeFeet = (obj.position.altitudeMeters * 3.281).roundToInt()
        "${obj.aircraftType ?: "Unknown"} - ${altitudeFeet}ft"
    }
    is Drone -> obj.manufacturer ?: "Unknown drone"
}

private fun Position.hasMapCoordinates(): Boolean = latitude != 0.0 || longitude != 0.0

private fun Position.toGeoPoint(): GeoPoint = GeoPoint(latitude, longitude)

private fun fovConePoints(center: GeoPoint, headingDegrees: Float, zoomLevel: Double): List<GeoPoint> {
    val radiusMeters = when {
        zoomLevel >= 14 -> 500.0
        zoomLevel >= 12 -> 1_500.0
        zoomLevel >= 10 -> 5_000.0
        else -> 15_000.0
    }
    val points = mutableListOf(center)
    for (step in 0..20) {
        val angle = headingDegrees - 30.0 + (60.0 * step / 20)
        points += offsetPoint(center, radiusMeters, Math.toRadians(angle))
    }
    points += center
    return points
}

private fun offsetPoint(center: GeoPoint, distanceMeters: Double, bearingRadians: Double): GeoPoint {
    val earthRadius = 6_371_000.0
    val latitude = Math.toRadians(center.latitude)
    val longitude = Math.toRadians(center.longitude)
    val angularDistance = distanceMeters / earthRadius
    val nextLatitude = Math.asin(
        sin(latitude) * cos(angularDistance) +
            cos(latitude) * sin(angularDistance) * cos(bearingRadians)
    )
    val nextLongitude = longitude + Math.atan2(
        sin(bearingRadians) * sin(angularDistance) * cos(latitude),
        cos(angularDistance) - sin(latitude) * sin(nextLatitude),
    )
    return GeoPoint(Math.toDegrees(nextLatitude), Math.toDegrees(nextLongitude))
}

private fun createUserDrawable(context: Context, following: Boolean): Drawable {
    val density = context.resources.displayMetrics.density
    val sizePx = ((if (following) 32 else 24) * density).toInt()
    val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)
    val center = sizePx / 2f

    if (following) {
        val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF2196F3.toInt()
            style = Paint.Style.FILL
        }
        val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFFFFFFFF.toInt()
            style = Paint.Style.STROKE
            strokeWidth = 2f * density
        }
        val unit = sizePx / 10f
        val path = Path().apply {
            moveTo(center, center - 4 * unit)
            lineTo(center + 3 * unit, center + 3 * unit)
            lineTo(center, center + 1.5f * unit)
            lineTo(center - 3 * unit, center + 3 * unit)
            close()
        }
        canvas.drawPath(path, fill)
        canvas.drawPath(path, border)
    } else {
        val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF2196F3.toInt()
            style = Paint.Style.FILL
        }
        val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFFFFFFFF.toInt()
            style = Paint.Style.STROKE
            strokeWidth = 3f * density
        }
        canvas.drawCircle(center, center, center - 2 * density, fill)
        canvas.drawCircle(center, center, center - 2 * density, border)
    }
    return BitmapDrawable(context.resources, bitmap)
}

private fun createSensorDrawable(context: Context): Drawable {
    val density = context.resources.displayMetrics.density
    val sizePx = (14 * density).toInt()
    val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)
    val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF4CAF50.toInt()
        style = Paint.Style.FILL
    }
    val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFFFFFF.toInt()
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    canvas.drawRect(2f, 2f, sizePx - 2f, sizePx - 2f, fill)
    canvas.drawRect(2f, 2f, sizePx - 2f, sizePx - 2f, border)
    return BitmapDrawable(context.resources, bitmap)
}

private fun createSensorDroneDrawable(context: Context, color: Int): Drawable {
    val density = context.resources.displayMetrics.density
    val sizePx = (18 * density).toInt()
    val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)
    val half = sizePx / 2f
    val path = Path().apply {
        moveTo(half, 2f)
        lineTo(sizePx - 2f, half)
        lineTo(half, sizePx - 2f)
        lineTo(2f, half)
        close()
    }
    val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color
        style = Paint.Style.FILL
    }
    val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = 0xFFFFFFFF.toInt()
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    canvas.drawPath(path, fill)
    canvas.drawPath(path, border)
    return BitmapDrawable(context.resources, bitmap)
}

private fun sensorDroneColor(drone: LocatedDroneDto): Int = when (drone.positionSource) {
    "gps" -> 0xFFF44336.toInt()
    "trilateration" -> 0xFFE91E63.toInt()
    "intersection" -> 0xFFFF9800.toInt()
    else -> 0xFFFF6D00.toInt()
}

private fun sensorDroneTitle(drone: LocatedDroneDto): String {
    val manufacturer = drone.manufacturer?.let { "$it " } ?: ""
    val label = "$manufacturer${drone.model ?: ""}".trim().ifEmpty { drone.droneId.take(20) }
    val method = when (drone.positionSource) {
        "gps" -> "GPS"
        "trilateration" -> "${drone.sensorCount}-sensor triangulated"
        "intersection" -> "2-sensor intersection"
        "range_only" -> "~${drone.rangeM?.toInt() ?: "?"}m range"
        else -> ""
    }
    return "$label ($method)"
}

private fun sensorDroneSnippet(drone: LocatedDroneDto): String {
    val parts = mutableListOf(
        "Sensors: ${drone.sensorCount}",
        "Confidence: ${"%.0f".format(drone.confidence * 100)}%",
    )
    drone.accuracyM?.let { parts += "Accuracy: ~${it.toInt()}m" }
    return parts.joinToString(", ")
}
