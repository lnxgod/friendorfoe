package com.friendorfoe.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import okhttp3.HttpUrl.Companion.toHttpUrl

class BackendEndpointTest {
    @Test
    fun rejectsCleartextEvenForUserSuppliedLocalServers() {
        listOf("http://192.168.4.20:8000", "http://192.168.4.1", "http://backend.example").forEach { url ->
            assertTrue(BackendEndpoint.parse(url).isFailure)
        }
    }

    @Test
    fun canonicalizesRootAndRemovesQueryAndFragment() {
        assertEquals(
            "https://field-kit.example:8443/",
            BackendEndpoint.parse(" https://field-kit.example:8443/api?q=1#status ")
                .getOrThrow()
                .baseUrl,
        )
    }

    @Test
    fun rejectsMissingHostCredentialsAndUnsupportedScheme() {
        listOf(
            "hello",
            "ftp://host/",
            "http://user:pass@host/",
            "http:///missing-host",
        ).forEach { raw ->
            assertTrue("Expected failure for $raw", BackendEndpoint.parse(raw).isFailure)
        }
    }

    @Test
    fun configuredRequestUsesEndpointOriginAndPreservesRequestPath() {
        assertEquals(
            "https://field-kit.example:8443/api/v1/health?verbose=true",
            configuredBackendRequestUrl(
                "https://field-kit.example:8443/old/path?discarded=true",
                "http://localhost:8000/api/v1/health?verbose=true".toHttpUrl(),
            ).toString(),
        )
    }

    @Test(expected = IOException::class)
    fun invalidLegacyValueFailsRequestInsteadOfKeepingLocalhost() {
        configuredBackendRequestUrl(
            "not a backend URL",
            "http://localhost:8000/api/v1/health".toHttpUrl(),
        )
    }
}
