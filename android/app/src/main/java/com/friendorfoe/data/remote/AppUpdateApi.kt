package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName
import retrofit2.http.GET

data class ReleaseMetadataDto(
    @SerializedName("tag_name") val tagName: String?,
    @SerializedName("version_code") val versionCode: Long?,
    @SerializedName("html_url") val htmlUrl: String?,
)

interface AppUpdateApi {
    @GET("repos/lnxgod/friendorfoe/releases/latest")
    suspend fun latestRelease(): ReleaseMetadataDto
}
