package com.friendorfoe.presentation.navigation

/**
 * Navigation destinations for the app.
 *
 * Three main screens:
 * - AR View (default): live camera viewfinder with floating labels
 * - List View: all detected objects sorted by distance
 * - History: past identifications from Room DB
 *
 * Plus a detail screen for tapping on a specific object.
 */
sealed class Screen(val route: String) {
    /** AR viewfinder with floating labels on sky objects */
    data object ArView : Screen("ar_view")

    /** 2D map view with aircraft markers on OpenStreetMap */
    data object MapView : Screen("map_view")

    /** List of all currently detected objects */
    data object ListView : Screen("list_view")

    /** Historical detection records */
    data object History : Screen("history")

    /** Detail card for a specific sky object */
    data object Detail : Screen("detail/{objectId}") {
        fun createRoute(objectId: String) = "detail/$objectId"
    }

    /** Drone reference guide */
    data object DroneGuide : Screen("drone_guide?manufacturer={manufacturer}") {
        fun createRoute(manufacturer: String? = null) =
            if (manufacturer != null) "drone_guide?manufacturer=$manufacturer"
            else "drone_guide"
    }

    /** Aircraft reference guide */
    data object AircraftGuide : Screen("aircraft_guide?type={type}") {
        fun createRoute(typeCode: String? = null) =
            if (typeCode != null) "aircraft_guide?type=$typeCode"
            else "aircraft_guide"
    }

    /** Combined reference guide with Aircraft + Drone tabs */
    data object ReferenceGuide : Screen("reference_guide")

    /** Privacy scanner screen */
    data object Privacy : Screen("privacy")

    /** Help / About screen */
    data object About : Screen("about")

    /** Welcome / launch screen */
    data object Welcome : Screen("welcome")
}
