package com.friendorfoe.presentation.detail

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.components.ReferenceImage
import com.friendorfoe.presentation.util.getAircraftPhotoUrl
import com.friendorfoe.presentation.util.silhouetteDrawableRes
import com.friendorfoe.presentation.util.silhouetteForCategory
import com.friendorfoe.presentation.util.silhouetteForTypeCode

@Composable
internal fun AircraftPhotoCard(visual: AircraftVisual, modifier: Modifier = Modifier) {
    val localPhoto = getAircraftPhotoUrl(visual.typeCode)
    val remotePhoto = visual.photoUrl?.takeIf(String::isNotBlank)
    ReferenceImage(
        model = remotePhoto ?: localPhoto,
        fallbackModel = localPhoto,
        description = visual.description ?: visual.typeCode ?: "Aircraft",
        silhouetteRes = silhouetteDrawableRes(
            silhouetteForTypeCode(visual.typeCode) ?: silhouetteForCategory(visual.category),
        ),
        modifier = modifier.fillMaxWidth().height(200.dp).clip(RoundedCornerShape(12.dp))
            .testTag("detail_aircraft_photo"),
        imageTag = "detail_aircraft_photo_image",
        fallbackTag = "detail_aircraft_silhouette",
        caption = if (remotePhoto == null) "Type reference" else null,
    )
}
