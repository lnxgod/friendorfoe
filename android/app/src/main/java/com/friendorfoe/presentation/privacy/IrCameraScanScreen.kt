package com.friendorfoe.presentation.privacy

import android.view.ViewGroup
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.ui.platform.testTag
import kotlin.math.roundToInt

data class IrActions(
    val onRetryBind: () -> Unit = {},
    val onOpenSettings: () -> Unit = {},
    val onBack: () -> Unit = {},
)

@Composable
fun IrCameraScanScreen(
    onBack: () -> Unit,
    viewModel: IrCameraScanViewModel = hiltViewModel(),
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val bindingGeneration by viewModel.bindingGeneration.collectAsStateWithLifecycle()
    val previewView = remember(context) {
        PreviewView(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            scaleType = PreviewView.ScaleType.FILL_CENTER
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
        }
    }
    val binder = remember(context) {
        CameraXIrCameraBinder(context.applicationContext)
    }
    val shouldBind = state !is IrCameraUiState.BindFailed

    DisposableEffect(
        lifecycleOwner,
        previewView,
        binder,
        bindingGeneration,
        shouldBind,
    ) {
        var bound = false
        fun bindIfNeeded() {
            if (!bound && shouldBind) {
                bound = true
                binder.bind(
                    lifecycleOwner = lifecycleOwner,
                    previewView = previewView,
                    onFrame = viewModel::onFrame,
                    onFailure = viewModel::onBindFailure,
                )
            }
        }

        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> bindIfNeeded()
                Lifecycle.Event.ON_STOP -> {
                    binder.unbind()
                    bound = false
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
            bindIfNeeded()
        }
        if (!shouldBind) binder.unbind()

        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            binder.unbind()
        }
    }

    IrCameraContent(
        state = state,
        actions = IrActions(
            onRetryBind = viewModel::retryBinding,
            onBack = onBack,
        ),
        previewContent = {
            AndroidView(
                factory = { previewView },
                modifier = Modifier.fillMaxSize(),
            )
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun IrCameraContent(
    state: IrCameraUiState,
    actions: IrActions,
    modifier: Modifier = Modifier,
    previewContent: @Composable () -> Unit = {},
) {
    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("IR-like light scan") },
                navigationIcon = {
                    IconButton(onClick = actions.onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                ),
            )
        },
    ) { innerPadding ->
        when (state) {
            is IrCameraUiState.BindFailed -> IrBindFailure(
                message = state.message,
                onRetry = actions.onRetryBind,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
            )
            IrCameraUiState.BindingCamera -> IrPreviewSurface(
                frame = null,
                previewContent = previewContent,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
            )
            is IrCameraUiState.Live -> IrPreviewSurface(
                frame = state.frame,
                previewContent = previewContent,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
            )
        }
    }
}

@Composable
private fun IrPreviewSurface(
    frame: IrPreviewFrame?,
    previewContent: @Composable () -> Unit,
    modifier: Modifier = Modifier,
) {
    BoxWithConstraints(
        modifier = modifier
            .testTag("ir_preview")
            .background(Color.Black),
    ) {
        previewContent()
        if (frame == null) {
            Column(
                modifier = Modifier.align(Alignment.Center),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                CircularProgressIndicator(color = Color.White)
                Text(
                    text = "Starting camera…",
                    color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                )
            }
        } else {
            BrightPointMarkers(frame)
            IrEvidenceBanner(
                frame = frame,
                modifier = Modifier.align(Alignment.TopCenter),
            )
            IrExplanationCard(
                frame = frame,
                modifier = Modifier.align(Alignment.BottomCenter),
            )
        }
    }
}

@Composable
private fun androidx.compose.foundation.layout.BoxWithConstraintsScope.BrightPointMarkers(
    frame: IrPreviewFrame,
) {
    val density = LocalDensity.current
    val markerSize = 34.dp
    val markerSizePx = with(density) { markerSize.roundToPx() }
    val surfaceWidthPx = constraints.maxWidth
    val surfaceHeightPx = constraints.maxHeight
    val mapped = frame.mappedCentersPx
        ?.takeIf { it.size == frame.analysis.sources.size }
        ?: frame.analysis.sources.mapNotNull { source ->
            runCatching {
                transformAnalysisPoint(
                    source = source.centerPx,
                    metadata = frame.metadata,
                    previewWidth = frame.previewWidthPx,
                    previewHeight = frame.previewHeightPx,
                )
            }.getOrNull()
        }

    mapped.forEachIndexed { index, point ->
        val displayX = point.x / frame.previewWidthPx * surfaceWidthPx
        val displayY = point.y / frame.previewHeightPx * surfaceHeightPx
        if (displayX in 0f..surfaceWidthPx.toFloat() &&
            displayY in 0f..surfaceHeightPx.toFloat()
        ) {
            val left = (displayX - markerSizePx / 2f)
                .roundToInt()
                .coerceIn(0, (surfaceWidthPx - markerSizePx).coerceAtLeast(0))
            val top = (displayY - markerSizePx / 2f)
                .roundToInt()
                .coerceIn(0, (surfaceHeightPx - markerSizePx).coerceAtLeast(0))
            Box(
                modifier = Modifier
                    .offset { IntOffset(left, top) }
                    .size(markerSize)
                    .testTag("ir_source_$index")
                    .semantics {
                        contentDescription = "Possible IR-like light ${index + 1}"
                    }
                    .background(Color(0x33FFEB3B), CircleShape)
                    .border(3.dp, Color(0xFFFFEB3B), CircleShape),
            )
        }
    }
}

@Composable
private fun IrEvidenceBanner(
    frame: IrPreviewFrame,
    modifier: Modifier = Modifier,
) {
    val sourceCount = frame.analysis.sources.size
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Color.Black.copy(alpha = 0.68f))
            .padding(horizontal = 18.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        Text(
            text = if (sourceCount == 0) {
                "No persistent bright points"
            } else {
                "Possible IR-like light · $sourceCount bright ${if (sourceCount == 1) "point" else "points"}"
            },
            color = Color.White,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
        )
        Text(
            text = if (frame.analysis.roomTooBright) {
                "The scene is bright, so faint points may be washed out."
            } else {
                "Ambient sample ${frame.analysis.ambientBrightness}/255"
            },
            color = if (frame.analysis.roomTooBright) Color(0xFFFFE082) else Color.White.copy(alpha = 0.78f),
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun IrExplanationCard(
    frame: IrPreviewFrame,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .widthIn(max = 680.dp)
            .fillMaxWidth()
            .background(
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.94f),
                shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
            )
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = "What this view can show",
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
        )
        Text(
            text = "Bright or blinking pixels can come from displays, LEDs, reflections, compression, or sensor noise. They do not identify a camera.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(
                text = if (frame.metadata.frontCamera) "Front camera" else "Back camera",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = "Evidence only",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}

@Composable
private fun IrBindFailure(
    message: String,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier, contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .widthIn(max = 520.dp)
                .fillMaxWidth()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text(
                text = "Camera could not start",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = message,
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = "This tool only highlights possible IR-like light; it does not identify a camera.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = onRetry) { Text("Retry") }
        }
    }
}
