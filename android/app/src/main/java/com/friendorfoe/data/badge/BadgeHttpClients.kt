package com.friendorfoe.data.badge

import okhttp3.OkHttpClient
import okhttp3.Request
import java.util.concurrent.TimeUnit

data class BadgeHttpClients(
    val status: OkHttpClient,
    val command: OkHttpClient,
)

data class BadgeHttpResponse(val code: Int, val body: String)

fun executeBadgeStatusCall(
    clients: BadgeHttpClients,
    request: Request,
): BadgeHttpResponse = clients.status.newCall(request).execute().use { response ->
    BadgeHttpResponse(response.code, response.body?.string().orEmpty())
}

fun executeBadgeCommandCall(
    clients: BadgeHttpClients,
    request: Request,
): BadgeHttpResponse = clients.command.newCall(request).execute().use { response ->
    BadgeHttpResponse(response.code, response.body?.string().orEmpty())
}

fun badgeHttpClients(base: OkHttpClient): BadgeHttpClients = BadgeHttpClients(
    status = base.newBuilder()
        .connectTimeout(1_200, TimeUnit.MILLISECONDS)
        .readTimeout(1_200, TimeUnit.MILLISECONDS)
        .writeTimeout(1_200, TimeUnit.MILLISECONDS)
        .callTimeout(1_500, TimeUnit.MILLISECONDS)
        .retryOnConnectionFailure(false)
        .build(),
    command = base.newBuilder()
        .connectTimeout(6_000, TimeUnit.MILLISECONDS)
        .readTimeout(6_000, TimeUnit.MILLISECONDS)
        .writeTimeout(6_000, TimeUnit.MILLISECONDS)
        .callTimeout(6_000, TimeUnit.MILLISECONDS)
        .retryOnConnectionFailure(false)
        .build(),
)

fun enforceAckDeadline(
    outcome: BadgeCommandOutcome,
    elapsedMs: Long,
): BadgeCommandOutcome = if (elapsedMs <= 5_000L) {
    outcome
} else {
    BadgeCommandOutcome.TimedOut
}
