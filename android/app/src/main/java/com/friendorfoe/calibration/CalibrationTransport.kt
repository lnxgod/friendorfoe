package com.friendorfoe.calibration

import java.io.IOException
import java.util.concurrent.TimeUnit
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.logging.HttpLoggingInterceptor

/** Tokens and precise walk locations must stay encrypted, including on a local network. */
internal object CalibrationTransport {
    private val endpointError = "Enter the final https:// backend URL. Calibration requires HTTPS, including on a local network."

    fun request(base: String, path: String, query: Map<String, String> = emptyMap()): Request.Builder {
        val url = base.trim().toHttpUrlOrNull() ?: throw IOException(endpointError)
        if (!url.isHttps || url.username.isNotEmpty() || url.password.isNotEmpty() || url.query != null || url.fragment != null) {
            throw IOException(endpointError)
        }
        val target = url.newBuilder().encodedPath(url.encodedPath.trimEnd('/') + path)
        query.forEach { (name, value) -> target.addQueryParameter(name, value) }
        return Request.Builder().url(target.build()).tag(HttpUrl::class.java, url)
    }

    fun client(shared: OkHttpClient): OkHttpClient = shared.newBuilder()
        .apply { interceptors().removeAll { it is HttpLoggingInterceptor } }
        // The shared backend interceptor enforces the opt-in and rewrites the URL
        // from current preferences. A settings change must not redirect an old token.
        .addInterceptor { chain ->
            val request = chain.request()
            val intended = request.tag(HttpUrl::class.java)
            if (!request.url.isHttps || intended == null || request.url.host != intended.host || request.url.port != intended.port) {
                throw IOException("The calibration endpoint changed. Check the HTTPS URL and token before starting again.")
            }
            chain.proceed(request)
        }
        .followRedirects(false)
        .followSslRedirects(false)
        .connectTimeout(20, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        .writeTimeout(20, TimeUnit.SECONDS)
        .callTimeout(30, TimeUnit.SECONDS)
        .build()
}
