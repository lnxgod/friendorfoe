package com.friendorfoe.data.remote

import com.friendorfoe.data.BackendEndpoint
import com.google.gson.Gson
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.OkHttpClient
import okhttp3.Request

data class BackendHealthResponse(
    val status: String,
    val version: String?,
)

fun interface BackendHealthClient {
    suspend fun check(endpoint: BackendEndpoint): BackendHealthResponse
}

class HttpBackendHealthClient(
    private val client: OkHttpClient,
    private val gson: Gson = Gson(),
) : BackendHealthClient {
    override suspend fun check(endpoint: BackendEndpoint): BackendHealthResponse =
        withContext(Dispatchers.IO) {
            val url = endpoint.baseUrl.toHttpUrl()
                .newBuilder()
                .addPathSegment("health")
                .build()
            val request = Request.Builder()
                .url(url)
                .header("Accept", "application/json")
                .get()
                .build()

            client.newCall(request).execute().use { response ->
                if (!response.isSuccessful) {
                    throw IOException("Backend health request failed")
                }
                val body = response.body?.string()
                    ?: throw IOException("Backend health response was empty")
                val payload = try {
                    gson.fromJson(body, HealthPayload::class.java)
                } catch (failure: Exception) {
                    throw IOException("Backend health response was invalid", failure)
                }
                if (!payload.status.equals("ok", ignoreCase = true)) {
                    throw IOException("Backend did not report healthy")
                }
                BackendHealthResponse(
                    status = "ok",
                    version = payload.version?.trim()?.takeIf(String::isNotEmpty),
                )
            }
        }

    private data class HealthPayload(
        val status: String?,
        val version: String?,
    )
}
