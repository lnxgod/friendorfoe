package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName
import retrofit2.http.GET
import retrofit2.http.Query

data class ReleaseAssetDto(
    val name: String?,
    val state: String?,
    val size: Long?,
    @SerializedName("browser_download_url") val downloadUrl: String?,
)

data class ReleaseMetadataDto(
    @SerializedName("tag_name") val tagName: String?,
    @SerializedName("version_code") val versionCode: Long?,
    @SerializedName("html_url") val htmlUrl: String?,
    val draft: Boolean = false,
    val prerelease: Boolean = false,
    val assets: List<ReleaseAssetDto>? = emptyList(),
)

interface AppUpdateApi {
    @GET("repos/lnxgod/friendorfoe/releases?per_page=100")
    suspend fun releases(@Query("page") page: Int): List<ReleaseMetadataDto>
}
