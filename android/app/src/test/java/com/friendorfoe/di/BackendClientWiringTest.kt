package com.friendorfoe.di

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.BackendRequestInterceptor
import com.friendorfoe.data.SensorBackendDisabledException
import kotlinx.coroutines.test.runTest
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class BackendClientWiringTest {
    @Test
    fun backendHealthUsesTheGuardedBackendClientProvider() = runTest {
        val backendClient = NetworkModule.provideBackendOkHttpClient(
            loggingInterceptor = HttpLoggingInterceptor(),
            backendRequestInterceptor = BackendRequestInterceptor(
                enabled = { false },
                configuredUrl = { "https://backend.example/" },
            ),
        )
        val health = InfoModule.provideBackendHealthClient(backendClient)
        val endpoint = BackendEndpoint.parse("https://backend.example/").getOrThrow()

        val failure = runCatching { health.check(endpoint) }.exceptionOrNull()

        assertTrue(failure is SensorBackendDisabledException)
    }

    @Test
    fun githubUpdateRetrofitKeepsTheGeneralUnguardedClient() {
        val generalClient = OkHttpClient()

        val retrofit = InfoModule.provideAppUpdateRetrofit(generalClient)

        assertSame(generalClient, retrofit.callFactory())
        assertEquals("https://api.github.com/", retrofit.baseUrl().toString())
    }
}
