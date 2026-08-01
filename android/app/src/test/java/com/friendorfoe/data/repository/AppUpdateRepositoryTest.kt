package com.friendorfoe.data.repository

import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.remote.AppUpdateApi
import com.friendorfoe.data.remote.ReleaseMetadataDto
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AppUpdateRepositoryTest {
    @Test
    fun validHttpsReleaseBecomesTypedMetadata() = runTest {
        val repository = HttpAppUpdateRepository(
            api = fixedApi(
                ReleaseMetadataDto(
                    tagName = "v0.65.0-release+1",
                    versionCode = 109,
                    htmlUrl = "https://github.com/lnxgod/friendorfoe/releases/tag/v0.65.0",
                ),
            ),
        )

        assertEquals(
            AppUpdateMetadata(
                version = AppVersion(109, "v0.65.0-release+1"),
                releaseUrl = "https://github.com/lnxgod/friendorfoe/releases/tag/v0.65.0",
            ),
            repository.latest().getOrThrow(),
        )
    }

    @Test
    fun malformedOrInsecureReleaseMetadataIsRejected() = runTest {
        val invalid = listOf(
            ReleaseMetadataDto(null, 109, "https://github.com/lnxgod/friendorfoe/releases"),
            ReleaseMetadataDto("latest", 109, "https://github.com/lnxgod/friendorfoe/releases"),
            ReleaseMetadataDto("0.65.0garbage", 109, "https://github.com/lnxgod/friendorfoe/releases"),
            ReleaseMetadataDto("0.65.0", 109, null),
            ReleaseMetadataDto("0.65.0", 109, "http://github.com/lnxgod/friendorfoe/releases"),
        )

        invalid.forEach { metadata ->
            assertTrue(
                "Expected invalid release metadata to fail: $metadata",
                HttpAppUpdateRepository(fixedApi(metadata)).latest().isFailure,
            )
        }
    }

    private fun fixedApi(metadata: ReleaseMetadataDto): AppUpdateApi =
        object : AppUpdateApi {
            override suspend fun latestRelease(): ReleaseMetadataDto = metadata
        }
}
