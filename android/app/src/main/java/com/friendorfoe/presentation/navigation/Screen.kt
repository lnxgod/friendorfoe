package com.friendorfoe.presentation.navigation

import android.net.Uri
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.ui.graphics.vector.ImageVector
import com.friendorfoe.presentation.badge.BadgeMarkIcon
import com.friendorfoe.presentation.privacy.PrivacyFindingKey
import com.friendorfoe.presentation.privacy.PrivacySourceKind

sealed class Screen(val route: String) {
    data object ArView : Screen("ar_view")
    data object MapView : Screen("map_view")
    data object ListView : Screen("list_view")
    data object Privacy : Screen("privacy")
    data object Badge : Screen("badge")
    data object BadgeFocus : Screen("badge/{focusKey}") {
        fun createRoute(focusKey: String) = "badge/${Uri.encode(focusKey)}"
    }
    data object History : Screen("history")
    data object About : Screen("info")
    data object AboutSettings : Screen("info/settings")
    data object Detail : Screen("detail/{objectId}") {
        fun createRoute(objectId: String) = "detail/${encodeRouteSegment(objectId)}"
    }
    data object HistoricalDetail : Screen("history_detail/{historyId}") {
        fun createRoute(historyId: Long) = "history_detail/$historyId"
    }
    data object ReferenceGuide : Screen("reference_guide?tab={tab}&query={query}")
    data object DroneGuide : Screen("drone_guide?manufacturer={manufacturer}") {
        fun createRoute(manufacturer: String? = null) = manufacturer?.let {
            "drone_guide?manufacturer=${encodeRouteSegment(it)}"
        } ?: "drone_guide"
    }
    data object AircraftGuide : Screen("aircraft_guide?type={type}") {
        fun createRoute(typeCode: String? = null) = typeCode?.let {
            "aircraft_guide?type=${encodeRouteSegment(it)}"
        } ?: "aircraft_guide"
    }
    data object IgnoredDevices : Screen("privacy/ignored")
    data object PrivacyFinding : Screen("privacy/finding/{source}/{record}") {
        fun createRoute(key: PrivacyFindingKey): String =
            "privacy/finding/${encodeRouteSegment(key.source.preferenceId)}/" +
                encodeRouteSegment(key.sourceRecordId)

        fun parseRoute(route: String): PrivacyFindingKey? {
            val segments = route.split('/')
            if (segments.size != 4 || segments[0] != "privacy" || segments[1] != "finding") {
                return null
            }
            val sourceId = decodeRouteSegment(segments[2]) ?: return null
            val record = decodeRouteSegment(segments[3])?.takeIf(String::isNotBlank) ?: return null
            val source = PrivacySourceKind.entries.singleOrNull {
                it.preferenceId == sourceId
            } ?: return null
            return runCatching { PrivacyFindingKey(source, record) }.getOrNull()
        }

        fun keyFromNavigationArguments(
            source: String?,
            record: String?,
        ): PrivacyFindingKey? {
            val sourceKind = PrivacySourceKind.entries.singleOrNull {
                it.preferenceId == source
            } ?: return null
            val exactRecord = record?.takeIf(String::isNotBlank) ?: return null
            return runCatching { PrivacyFindingKey(sourceKind, exactRecord) }.getOrNull()
        }
    }
    data object EmfSweep : Screen("info/advanced/magnetic_field")
    data object IrCameraScan : Screen("info/advanced/ir_light")
    data object Calibrate : Screen("info/advanced/calibrate")
}

enum class TopLevelDestination(
    val label: String,
    val route: String,
    val icon: ImageVector,
) {
    AR("AR", Screen.ArView.route, Icons.Default.Visibility),
    MAP("Map", Screen.MapView.route, Icons.Default.Map),
    LIST("List", Screen.ListView.route, Icons.AutoMirrored.Filled.List),
    PRIVACY("Privacy", Screen.Privacy.route, Icons.Default.Shield),
    BADGE("Badge", Screen.Badge.route, BadgeMarkIcon),
    HISTORY("History", Screen.History.route, Icons.Default.History),
    ABOUT("About", Screen.About.route, Icons.Default.Info),
}

enum class BackDisposition { EXIT_APP, POP_SECONDARY }

fun backDisposition(route: String?): BackDisposition =
    if (route in TopLevelDestination.entries.map { it.route }) BackDisposition.EXIT_APP
    else BackDisposition.POP_SECONDARY
