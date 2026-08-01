package com.friendorfoe.presentation.privacy

import com.friendorfoe.presentation.navigation.Screen
import java.net.URI
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

object PrivacyLaunchRoute {
    fun parse(dataUri: String?, routeExtra: String?): String? {
        val fromExtra = routeExtra?.let(Screen.PrivacyFinding::parseRoute)
        val fromData = dataUri?.let(::parseDataUri)
        if (routeExtra != null && fromExtra == null) return null
        if (dataUri != null && fromData == null) return null
        val key = when {
            fromExtra != null && fromData != null && fromExtra == fromData -> fromExtra
            fromExtra != null && dataUri == null -> fromExtra
            fromData != null && routeExtra == null -> fromData
            else -> return null
        }
        return Screen.PrivacyFinding.createRoute(key)
    }

    private fun parseDataUri(value: String): PrivacyFindingKey? = runCatching {
        val uri = URI(value)
        if (!uri.scheme.equals("friendorfoe", ignoreCase = true) ||
            uri.host != "privacy" ||
            uri.rawQuery != null ||
            uri.rawFragment != null
        ) {
            return@runCatching null
        }
        val rawPath = uri.rawPath?.removePrefix("/") ?: return@runCatching null
        Screen.PrivacyFinding.parseRoute("privacy/$rawPath")
    }.getOrNull()
}

class PendingPrivacyRouteQueue(restoredRoute: String? = null) {
    private val lock = Any()
    private val _pending = MutableStateFlow<String?>(null)
    val pending = _pending.asStateFlow()

    init {
        restoredRoute?.let { offer(dataUri = null, routeExtra = it) }
    }

    fun offer(dataUri: String?, routeExtra: String?): Boolean {
        val route = PrivacyLaunchRoute.parse(dataUri, routeExtra) ?: return false
        synchronized(lock) { _pending.value = route }
        return true
    }

    fun consume(route: String): Boolean = synchronized(lock) {
        if (_pending.value != route) return@synchronized false
        _pending.value = null
        true
    }

    fun savedRoute(): String? = synchronized(lock) { _pending.value }
}

data class PrivacyLaunchIntentPayload(
    val dataUri: String?,
    val routeExtra: String?,
)

fun acceptPrivacyLaunchIntent(
    queue: PendingPrivacyRouteQueue,
    payload: PrivacyLaunchIntentPayload,
): PrivacyLaunchIntentPayload = if (queue.offer(payload.dataUri, payload.routeExtra)) {
    PrivacyLaunchIntentPayload(dataUri = null, routeExtra = null)
} else {
    payload
}
