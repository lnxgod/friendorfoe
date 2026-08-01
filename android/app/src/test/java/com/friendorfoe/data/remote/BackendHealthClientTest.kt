package com.friendorfoe.data.remote

import com.friendorfoe.data.BackendEndpoint
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BackendHealthClientTest {
    @Test
    fun requestIsBoundToPassedEndpointAndParsesHealthyResponse() = runTest {
        var requestedUrl: String? = null
        val client = OkHttpClient.Builder()
            .addInterceptor { chain ->
                requestedUrl = chain.request().url.toString()
                response(
                    chain.request(),
                    code = 200,
                    body = """{"status":"ok","version":"0.64.65"}""",
                )
            }
            .build()
        val healthClient = HttpBackendHealthClient(client)
        val endpoint = BackendEndpoint.parse("https://field-kit.example:8443/old/path?q=x")
            .getOrThrow()

        assertEquals(
            BackendHealthResponse(status = "ok", version = "0.64.65"),
            healthClient.check(endpoint),
        )
        assertEquals("https://field-kit.example:8443/health", requestedUrl)
    }

    @Test
    fun httpFailureAndNonHealthyPayloadBothFailClosed() = runTest {
        val endpoint = BackendEndpoint.parse("https://field-kit.example/").getOrThrow()
        val httpFailure = HttpBackendHealthClient(
            clientResponding(code = 503, body = """{"status":"ok"}"""),
        )
        val unhealthyPayload = HttpBackendHealthClient(
            clientResponding(code = 200, body = """{"status":"degraded"}"""),
        )

        assertTrue(runCatching { httpFailure.check(endpoint) }.isFailure)
        assertTrue(runCatching { unhealthyPayload.check(endpoint) }.isFailure)
    }

    private fun clientResponding(code: Int, body: String): OkHttpClient =
        OkHttpClient.Builder()
            .addInterceptor { chain -> response(chain.request(), code, body) }
            .build()

    private fun response(
        request: okhttp3.Request,
        code: Int,
        body: String,
    ): Response = Response.Builder()
        .request(request)
        .protocol(Protocol.HTTP_1_1)
        .code(code)
        .message(if (code in 200..299) "OK" else "Failure")
        .body(body.toResponseBody("application/json".toMediaType()))
        .build()
}
