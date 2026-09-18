package com.friendorfoe.data.repository

import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.isUpdateAvailable
import com.friendorfoe.data.isWellFormedAppVersionName
import com.friendorfoe.data.remote.AppUpdateApi
import com.friendorfoe.data.remote.ReleaseMetadataDto
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

data class AppUpdateMetadata(
    val version: AppVersion,
    val releaseUrl: String,
    val apkUrl: String? = null,
)

interface AppUpdateRepository {
    suspend fun latest(): Result<AppUpdateMetadata>
}

class HttpAppUpdateRepository @Inject constructor(
    private val api: AppUpdateApi,
) : AppUpdateRepository {
    override suspend fun latest(): Result<AppUpdateMetadata> = try {
        var selected: AppUpdateMetadata? = null
        // Firmware and dashboard releases share this repository. Only offer a ready APK.
        // Bound requests while allowing a page of firmware releases ahead of Android.
        for (page in 1..3) {
            val releases = api.releases(page)
            releases.mapNotNull { it.toAndroidMetadata() }.forEach { candidate ->
                val current = selected
                if (current == null || isUpdateAvailable(current.version, candidate.version)) selected = candidate
            }
            if (selected != null || releases.size < 100) break
        }
        Result.success(requireNotNull(selected) { "No published Android APK is available" })
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (failure: Exception) {
        Result.failure(failure)
    }

    private fun ReleaseMetadataDto.toAndroidMetadata(): AppUpdateMetadata? {
        if (draft || prerelease) return null
        val tag = tagName?.trim().orEmpty()
        if (!isWellFormedAppVersionName(tag) || (versionCode != null && versionCode < 0)) return null
        val expectedRelease = "$RELEASE_BASE/tag/$tag"
        val releaseUrl = htmlUrl?.takeIf { it.isOfficialUrl(expectedRelease) } ?: return null
        val filename = "friendorfoe-$tag.apk"
        val expectedDownload = "$RELEASE_BASE/download/$tag/$filename"
        val apk = assets.orEmpty().singleOrNull {
            it.name == filename && it.state == "uploaded" && (it.size ?: 0) > 0 &&
                it.downloadUrl?.isOfficialUrl(expectedDownload) == true
        } ?: return null
        return AppUpdateMetadata(
            version = AppVersion(code = versionCode, name = tag),
            releaseUrl = releaseUrl,
            apkUrl = apk.downloadUrl,
        )
    }

    private fun String.isOfficialUrl(expected: String): Boolean {
        val url = toHttpUrlOrNull() ?: return false
        val expectedUrl = expected.toHttpUrlOrNull() ?: return false
        return url == expectedUrl && url.username.isEmpty() && url.password.isEmpty()
    }

    private companion object {
        const val RELEASE_BASE = "https://github.com/lnxgod/friendorfoe/releases"
    }
}
