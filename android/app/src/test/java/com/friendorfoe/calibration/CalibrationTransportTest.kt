package com.friendorfoe.calibration

import java.io.IOException
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okhttp3.logging.HttpLoggingInterceptor
import org.junit.Assert.*
import org.junit.Test

class CalibrationTransportTest {
    @Test fun requiresExplicitTlsAndRejectsCredentialsInEndpoint() {
        for (base in listOf("http://10.0.0.2:8000", "http://backend.example", "backend.example",
                "https://user:pass@backend.example", "https://backend.example/?token=secret")) {
            assertThrows(IOException::class.java) { CalibrationTransport.request(base, "/health") }
        }
        val req = CalibrationTransport.request("https://backend.example/prefix/", "/feedback",
            mapOf("session_id" to "one&admin=true", "window_s" to "10")).build()
        assertEquals("/prefix/feedback", req.url.encodedPath)
        assertEquals("one&admin=true", req.url.queryParameter("session_id"))
        assertNull(req.url.queryParameter("admin"))
        assertEquals("10", req.url.queryParameter("window_s"))
    }

    @Test fun changedOriginOrDowngradeCannotSendTokenAndCoordinates() {
        val intended = "https://backend.example/"
        for (other in listOf("http://backend.example/", "https://other.example/", "https://backend.example:8888/")) {
            val shared = OkHttpClient.Builder().addInterceptor { chain ->
                chain.proceed(chain.request().newBuilder().url(other.toHttpUrl()).build())
            }.addInterceptor(HttpLoggingInterceptor()).build()
            val guarded = CalibrationTransport.client(shared)
            assertFalse(guarded.followRedirects)
            assertFalse(guarded.followSslRedirects)
            assertFalse(guarded.interceptors.any { it is HttpLoggingInterceptor })
            val client = guarded.newBuilder().addInterceptor { error("Sensitive request escaped its selected endpoint") }.build()
            val req = CalibrationTransport.request(intended, "/walk/sample").header("X-Cal-Token", "test").build()
            assertThrows(IOException::class.java) { client.newCall(req).execute() }
        }
    }

    @Test fun stableOriginUsesTheNormalBackendOptInAndDeliversTheRequest() {
        var sharedGuardRan = false
        val shared = OkHttpClient.Builder().addInterceptor { chain ->
            sharedGuardRan = true
            chain.proceed(chain.request())
        }.build()
        val client = CalibrationTransport.client(shared).newBuilder().addInterceptor { chain ->
            assertEquals("test", chain.request().header("X-Cal-Token"))
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK").body("{}".toResponseBody()).build()
        }.build()
        val request = CalibrationTransport.request("https://backend.example", "/walk/sensors").header("X-Cal-Token", "test").build()
        client.newCall(request).execute().use { assertTrue(it.isSuccessful) }
        assertTrue(sharedGuardRan)
    }
}
