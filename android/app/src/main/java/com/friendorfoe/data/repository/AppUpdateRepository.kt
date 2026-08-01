package com.friendorfoe.data.repository

import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.isWellFormedAppVersionName
import com.friendorfoe.data.remote.AppUpdateApi
import com.friendorfoe.data.remote.ReleaseMetadataDto
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

data class AppUpdateMetadata(
    val version: AppVersion,
    val releaseUrl: String,
)

interface AppUpdateRepository {
    suspend fun latest(): Result<AppUpdateMetadata>
}

class HttpAppUpdateRepository @Inject constructor(
    private val api: AppUpdateApi,
) : AppUpdateRepository {
    override suspend fun latest(): Result<AppUpdateMetadata> = try {
        Result.success(api.latestRelease().toMetadata())
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (failure: Exception) {
        Result.failure(failure)
    }

    private fun ReleaseMetadataDto.toMetadata(): AppUpdateMetadata {
        val tag = tagName?.trim().orEmpty()
        require(isWellFormedAppVersionName(tag)) { "Release version is invalid" }
        require(versionCode == null || versionCode >= 0L) { "Release code is invalid" }

        val url = htmlUrl?.trim()?.toHttpUrlOrNull()
            ?: throw IllegalArgumentException("Release URL is missing")
        require(url.scheme == "https") { "Release URL must use HTTPS" }
        require(url.username.isBlank() && url.password.isBlank()) {
            "Release URL must not contain credentials"
        }

        return AppUpdateMetadata(
            version = AppVersion(code = versionCode, name = tag),
            releaseUrl = url.toString(),
        )
    }
}
