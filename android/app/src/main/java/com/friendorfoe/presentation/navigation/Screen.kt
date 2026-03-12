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

    /** List of all currently detected objects */
    data object ListView : Screen("list_view")

    /** Historical detection records */
    data object History : Screen("history")

    /** Detail card for a specific sky object */
    data object Detail : Screen("detail/{objectId}") {
        fun createRoute(objectId: String) = "detail/$objectId"
    }
}
