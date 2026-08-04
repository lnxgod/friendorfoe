package com.friendorfoe.data

import java.io.IOException
import okhttp3.Interceptor
import okhttp3.Response

internal class SensorBackendDisabledException : IOException(
    "Sensor backend is disabled",
)

internal class BackendRequestInterceptor(
    private val enabled: () -> Boolean,
    private val configuredUrl: () -> String,
) : Interceptor {
    constructor(prefs: DetectionPrefs) : this(
        enabled = { prefs.sensorBackendEnabled },
        configuredUrl = { prefs.backendUrl },
    )

    override fun intercept(chain: Interceptor.Chain): Response {
        if (!enabled()) throw SensorBackendDisabledException()
        val original = chain.request()
        val rewritten = configuredBackendRequestUrl(configuredUrl(), original.url)
        return chain.proceed(original.newBuilder().url(rewritten).build())
    }
}
