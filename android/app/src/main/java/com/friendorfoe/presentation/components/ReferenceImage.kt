package com.friendorfoe.presentation.components

import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import coil.compose.SubcomposeAsyncImageContent

/** Keeps the whole airframe visible, with a local fallback when a remote photo fails. */
@Composable
internal fun ReferenceImage(
    model: String?,
    description: String,
    @DrawableRes silhouetteRes: Int,
    modifier: Modifier = Modifier,
    fallbackModel: String? = null,
    imageTag: String = "reference_photo_image",
    fallbackTag: String = "reference_photo_fallback",
    caption: String? = "Reference photo",
) {
    Box(modifier.background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
        if (model.isNullOrBlank()) {
            ReferenceImageFallback(description, silhouetteRes, fallbackTag)
        } else {
            SubcomposeAsyncImage(
                model = model,
                contentDescription = description,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit,
                loading = {
                    if (!fallbackModel.isNullOrBlank() && fallbackModel != model) {
                        ReferenceImage(
                            model = fallbackModel,
                            description = description,
                            silhouetteRes = silhouetteRes,
                            modifier = Modifier.fillMaxSize(),
                            imageTag = imageTag,
                            fallbackTag = fallbackTag,
                            caption = "Type reference",
                        )
                    } else {
                        ReferenceImageFallback(description, silhouetteRes, fallbackTag, loading = true)
                    }
                },
                error = {
                    if (!fallbackModel.isNullOrBlank() && fallbackModel != model) {
                        ReferenceImage(
                            model = fallbackModel,
                            description = description,
                            silhouetteRes = silhouetteRes,
                            modifier = Modifier.fillMaxSize(),
                            imageTag = imageTag,
                            fallbackTag = fallbackTag,
                            caption = "Type reference",
                        )
                    } else {
                        ReferenceImageFallback(description, silhouetteRes, fallbackTag)
                    }
                },
                success = {
                    val imageScope = this
                    Box(Modifier.fillMaxSize()) {
                        imageScope.SubcomposeAsyncImageContent(Modifier.fillMaxSize().testTag(imageTag))
                        caption?.let {
                            Text(
                                it,
                                Modifier.align(Alignment.BottomStart)
                                    .background(Color.Black.copy(alpha = 0.72f))
                                    .padding(horizontal = 10.dp, vertical = 4.dp),
                                style = MaterialTheme.typography.labelSmall,
                                color = Color.White,
                            )
                        }
                    }
                },
            )
        }
    }
}

@Composable
private fun ReferenceImageFallback(
    description: String,
    @DrawableRes silhouetteRes: Int,
    tag: String,
    loading: Boolean = false,
) {
    Column(
        Modifier.fillMaxSize().testTag(tag).padding(16.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Image(
            painterResource(silhouetteRes),
            contentDescription = "$description · category illustration",
            modifier = Modifier.weight(1f, fill = false).fillMaxWidth(0.6f).heightIn(max = 100.dp),
            contentScale = ContentScale.Fit,
            colorFilter = ColorFilter.tint(MaterialTheme.colorScheme.primary),
        )
        Spacer(Modifier.height(12.dp))
        Text(
            if (loading) "Loading photo…" else "Photo unavailable",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
