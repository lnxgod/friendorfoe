package com.friendorfoe.data.repository

import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.remote.AppUpdateApi
import com.friendorfoe.data.remote.ReleaseAssetDto
import com.friendorfoe.data.remote.ReleaseMetadataDto
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.*
import org.junit.Test
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory

class AppUpdateRepositoryTest {
    private val base = "https://github.com/lnxgod/friendorfoe/releases"
    private fun release(tag: String = "v0.67.20-android-review") = ReleaseMetadataDto(
        tag, null, "$base/tag/$tag", assets = listOf(ReleaseAssetDto(
            "friendorfoe-$tag.apk", "uploaded", 90_000_000,
            "$base/download/$tag/friendorfoe-$tag.apk",
        )),
    )
    private fun repository(vararg releases: ReleaseMetadataDto) = HttpAppUpdateRepository(object : AppUpdateApi {
        override suspend fun releases(page: Int) = releases.toList()
    })

    @Test fun selectsHighestReadyAndroidVersionInsteadOfLatestFirmwareOrDraft() = runTest {
        val latest = release()
        val result = repository(
            release("v0.99.0-new-dash").copy(assets = emptyList()),
            release("v0.98.0-android").copy(prerelease = true),
            release("v0.97.0-android").copy(draft = true),
            release("v0.67.9-android"), latest, release("v0.67.19-android"),
        ).latest().getOrThrow()
        assertEquals(AppVersion(null, latest.tagName!!), result.version)
        assertEquals(latest.assets!!.single().downloadUrl, result.apkUrl)
        assertEquals(latest.htmlUrl, result.releaseUrl)
    }

    @Test fun ignoresMissingIncompleteAmbiguousAndWronglyNamedAssets() = runTest {
        val good = release()
        val asset = good.assets!!.single()
        val invalid = listOf(
            good.copy(assets = null), good.copy(assets = emptyList()),
            good.copy(assets = listOf(asset.copy(state = "starter"))),
            good.copy(assets = listOf(asset.copy(size = 0))),
            good.copy(assets = listOf(asset.copy(name = "app-debug.apk"))),
            good.copy(assets = listOf(asset, asset)),
        )
        invalid.forEach { assertTrue(repository(it).latest().isFailure) }
    }

    @Test fun rejectsMalformedVersionsAndUnofficialReleaseOrDownloadUrls() = runTest {
        val good = release()
        val asset = good.assets!!.single()
        val invalid = listOf(
            good.copy(tagName = "latest"), good.copy(tagName = null), good.copy(versionCode = -1),
            good.copy(htmlUrl = "https://example.com/releases/tag/${good.tagName}"),
            good.copy(htmlUrl = good.htmlUrl!!.replace("https:", "http:")),
            good.copy(htmlUrl = good.htmlUrl + "?redirect=other"),
            good.copy(assets = listOf(asset.copy(downloadUrl = asset.downloadUrl!!.replace("github.com", "example.com")))),
            good.copy(assets = listOf(asset.copy(downloadUrl = asset.downloadUrl!!.replace("github.com", "user@github.com")))),
            good.copy(assets = listOf(asset.copy(downloadUrl = asset.downloadUrl + "#other"))),
        )
        invalid.forEach { assertTrue("Should reject $it", repository(it).latest().isFailure) }
    }

    @Test fun searchesNextPageWhenFirmwareFillsTheFirstPage() = runTest {
        val calls = mutableListOf<Int>()
        val api = object : AppUpdateApi {
            override suspend fun releases(page: Int): List<ReleaseMetadataDto> {
                calls.add(page)
                return if (page == 1) List(100) { release().copy(assets = emptyList()) } else listOf(release())
            }
        }
        assertTrue(HttpAppUpdateRepository(api).latest().isSuccess)
        assertEquals(listOf(1, 2), calls)
    }

    @Test fun boundsPaginationAndReportsUnavailableInsteadOfUpToDate() = runTest {
        var calls = 0
        val api = object : AppUpdateApi {
            override suspend fun releases(page: Int): List<ReleaseMetadataDto> {
                calls++
                return List(100) { release().copy(assets = emptyList()) }
            }
        }
        assertTrue(HttpAppUpdateRepository(api).latest().isFailure)
        assertEquals(3, calls)
        assertTrue(repository().latest().isFailure)
    }

    @Test fun cancellationPropagates() = runTest {
        val api = object : AppUpdateApi {
            override suspend fun releases(page: Int): List<ReleaseMetadataDto> = throw CancellationException("cancelled")
        }
        try {
            HttpAppUpdateRepository(api).latest()
            fail("Cancellation must propagate")
        } catch (_: CancellationException) { }
    }

    @Test fun actualGithubJsonAndPagedEndpointProduceDirectApkMetadata() = runTest {
        val tag = "v0.67.20-android-review"
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            assertEquals("/repos/lnxgod/friendorfoe/releases", chain.request().url.encodedPath)
            assertEquals("100", chain.request().url.queryParameter("per_page"))
            assertEquals("1", chain.request().url.queryParameter("page"))
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body("""[{"tag_name":"$tag","html_url":"$base/tag/$tag","draft":false,"prerelease":false,"assets":[{"name":"friendorfoe-$tag.apk","state":"uploaded","size":12345,"browser_download_url":"$base/download/$tag/friendorfoe-$tag.apk"}]}]""".toResponseBody("application/json".toMediaType())).build()
        }.build()
        val api = Retrofit.Builder().baseUrl("https://api.github.com/").client(client)
            .addConverterFactory(GsonConverterFactory.create()).build().create(AppUpdateApi::class.java)
        val result = HttpAppUpdateRepository(api).latest().getOrThrow()
        assertEquals("$base/download/$tag/friendorfoe-$tag.apk", result.apkUrl)
        assertEquals(AppVersion(null, tag), result.version)
    }
}
