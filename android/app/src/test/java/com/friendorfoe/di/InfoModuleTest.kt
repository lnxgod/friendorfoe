package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.BackendEndpoint
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test

class InfoModuleTest {
    @Test
    fun updateApiUsesTheGitHubApiOrigin() {
        val retrofit = InfoModule.provideAppUpdateRetrofit(OkHttpClient())

        assertEquals("https://api.github.com/", retrofit.baseUrl().toString())
        assertNotNull(InfoModule.provideAppUpdateApi(retrofit))
    }

    @Test
    fun backendHealthUsesThePassedBaseClientWithoutBackendUrlRewriting() = runTest {
        var requestedUrl: String? = null
        val baseClient = OkHttpClient.Builder()
            .addInterceptor { chain ->
                requestedUrl = chain.request().url.toString()
                Response.Builder()
                    .request(chain.request())
                    .protocol(Protocol.HTTP_1_1)
                    .code(200)
                    .message("OK")
                    .body(
                        """{"status":"ok","version":"0.65.0"}"""
                            .toResponseBody("application/json".toMediaType()),
                    )
                    .build()
            }
            .build()
        val endpoint = BackendEndpoint.parse("https://field-kit.example:8443/").getOrThrow()

        InfoModule.provideBackendHealthClient(baseClient).check(endpoint)

        assertEquals("https://field-kit.example:8443/health", requestedUrl)
    }

    @Test
    fun installedVersionComesFromBuildMetadata() {
        assertEquals(
            AppVersion(BuildConfig.VERSION_CODE.toLong(), BuildConfig.VERSION_NAME),
            InfoModule.provideInstalledAppVersion(),
        )
    }
}
