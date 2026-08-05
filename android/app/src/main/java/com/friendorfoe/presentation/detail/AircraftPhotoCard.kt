package com.friendorfoe.presentation.detail

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImagePainter
import coil.compose.SubcomposeAsyncImage
import coil.compose.SubcomposeAsyncImageContent
import com.friendorfoe.presentation.util.categoryColor
import com.friendorfoe.presentation.util.getAircraftPhotoUrl
import com.friendorfoe.presentation.util.silhouetteDrawableRes
import com.friendorfoe.presentation.util.silhouetteForCategory
import com.friendorfoe.presentation.util.silhouetteForTypeCode

@Composable
internal fun AircraftPhotoCard(
    visual: AircraftVisual,
    modifier: Modifier = Modifier,
) {
    val silhouette = silhouetteForTypeCode(visual.typeCode)
        ?: silhouetteForCategory(visual.category)
    val drawableRes = silhouetteDrawableRes(silhouette)
    val tintColor = categoryColor(visual.category)
    val imageUrl = visual.photoUrl?.takeIf(String::isNotBlank)
        ?: getAircraftPhotoUrl(visual.typeCode)

    Card(
        modifier = modifier
            .fillMaxWidth()
            .height(180.dp)
            .testTag("detail_aircraft_photo"),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            if (imageUrl != null) {
                SubcomposeAsyncImage(
                    model = imageUrl,
                    contentDescription = visual.description
                        ?: visual.typeCode
                        ?: "Aircraft",
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                ) {
                    when (painter.state) {
                        is AsyncImagePainter.State.Success -> {
                            SubcomposeAsyncImageContent(
                                modifier = Modifier
                                    .fillMaxSize()
                                    .clip(RoundedCornerShape(12.dp))
                                    .testTag("detail_aircraft_photo_image"),
                            )
                        }
                        is AsyncImagePainter.State.Loading -> {
                            AircraftSilhouetteFallback(
                                drawableRes,
                                tintColor,
                                visual.typeCode,
                            )
                        }
                        else -> {
                            AircraftSilhouetteFallback(
                                drawableRes,
                                tintColor,
                                visual.typeCode,
                            )
                        }
                    }
                }
            } else {
                AircraftSilhouetteFallback(
                    drawableRes,
                    tintColor,
                    visual.typeCode,
                )
            }
        }
    }
}

@Composable
private fun AircraftSilhouetteFallback(
    drawableRes: Int,
    tintColor: Color,
    aircraftType: String?,
) {
    Column(
        modifier = Modifier.testTag("detail_aircraft_silhouette"),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Image(
            painter = painterResource(id = drawableRes),
            contentDescription = aircraftType ?: "Aircraft",
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .height(120.dp),
            contentScale = ContentScale.Fit,
            colorFilter = ColorFilter.tint(tintColor),
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = aircraftType ?: "Unknown",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
        )
    }
}
