package com.friendorfoe.data

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import java.io.IOException
import java.net.URI

@JvmInline
value class BackendEndpoint private constructor(val baseUrl: String) {
    companion object {
        fun parse(raw: String): Result<BackendEndpoint> = runCatching {
            val trimmed = raw.trim()
            val strict = URI(trimmed)
            require(strict.scheme == "http" || strict.scheme == "https")
            require(!strict.host.isNullOrBlank())
            require(strict.rawUserInfo == null)

            val parsed = trimmed.toHttpUrlOrNull()
                ?: error("Enter a complete http:// or https:// URL")
            require(parsed.scheme == "http" || parsed.scheme == "https")
            require(parsed.host.isNotBlank())
            require(parsed.username.isBlank() && parsed.password.isBlank())

            BackendEndpoint(
                parsed.newBuilder()
                    .encodedPath("/")
                    .query(null)
                    .fragment(null)
                    .build()
                    .toString(),
            )
        }
    }
}

internal fun configuredBackendRequestUrl(raw: String, original: HttpUrl): HttpUrl {
    val endpoint = BackendEndpoint.parse(raw).getOrElse { cause ->
        throw IOException("Configured backend URL is invalid", cause)
    }
    val base = endpoint.baseUrl.toHttpUrlOrNull()
        ?: throw IOException("Configured backend URL is invalid")
    return original.newBuilder()
        .scheme(base.scheme)
        .host(base.host)
        .port(base.port)
        .build()
}
