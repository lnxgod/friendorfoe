package com.friendorfoe.presentation.privacy

import com.friendorfoe.presentation.navigation.Screen

data class PrivacyNotificationRoute(
    val route: String,
    val dataUri: String,
    val pendingIntentId: Int,
) {
    companion object {
        const val EXTRA_ROUTE = "com.friendorfoe.extra.PRIVACY_FINDING_ROUTE"

        fun from(
            key: PrivacyFindingKey,
            ids: PrivacyNotificationIdStore,
        ): PrivacyNotificationRoute {
            val route = Screen.PrivacyFinding.createRoute(key)
            return PrivacyNotificationRoute(
                route = route,
                dataUri = "friendorfoe://$route",
                pendingIntentId = ids.idFor(key),
            )
        }
    }
}
