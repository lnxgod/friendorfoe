package com.friendorfoe.data

import java.io.IOException
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class BackendRequestInterceptorTest {
    @Test
    fun disabledBackendStopsBeforeDownstreamNetworkChain() {
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { false },
                configuredUrl = { "https://backend.example/" },
            ),
        ) { downstreamCalls++ }

        assertThrows(SensorBackendDisabledException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(0, downstreamCalls)
    }

    @Test
    fun enabledBackendUsesConfiguredOriginAndPreservesPath() {
        var observedUrl: String? = null
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { true },
                configuredUrl = { "https://field-kit.example:8443/" },
            ),
        ) { request -> observedUrl = request.url.toString() }

        client.newCall(request("http://localhost:8000/detections/drone-alerts?q=1"))
            .execute().close()

        assertEquals(
            "https://field-kit.example:8443/detections/drone-alerts?q=1",
            observedUrl,
        )
    }

    @Test
    fun invalidConfiguredUrlFailsBeforeDownstreamChain() {
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { true },
                configuredUrl = { "not a url" },
            ),
        ) { downstreamCalls++ }

        assertThrows(IOException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(0, downstreamCalls)
    }

    @Test
    fun disablingPreferenceBlocksTheNextRequestWithoutRebuildingClient() {
        var enabled = true
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { enabled },
                configuredUrl = { "https://backend.example/" },
            ),
        ) { downstreamCalls++ }

        client.newCall(request()).execute().close()
        enabled = false

        assertThrows(SensorBackendDisabledException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(1, downstreamCalls)
    }
}

private fun request(
    url: String = "http://localhost:8000/health",
): Request = Request.Builder().url(url).build()

private fun testClient(
    gate: Interceptor,
    onDownstream: (Request) -> Unit,
): OkHttpClient = OkHttpClient.Builder()
    .addInterceptor(gate)
    .addInterceptor { chain ->
        onDownstream(chain.request())
        Response.Builder()
            .request(chain.request())
            .protocol(Protocol.HTTP_1_1)
            .code(200)
            .message("OK")
            .body("{}".toResponseBody("application/json".toMediaType()))
            .build()
    }
    .build()
