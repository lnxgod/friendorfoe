package com.friendorfoe.presentation.privacy

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.util.Size
import android.view.ViewGroup
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun IrCameraScanScreen(
    onBack: () -> Unit,
    viewModel: IrCameraScanViewModel = hiltViewModel()
) {
    val context = LocalContext.current
    var cameraLens by remember { mutableStateOf(IrCameraLens.STARTING) }
    var cameraGranted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> cameraGranted = granted }

    LaunchedEffect(Unit) {
        viewModel.reset()
        if (!cameraGranted) {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("IR Camera Scan") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    Button(onClick = viewModel::reset, modifier = Modifier.padding(end = 8.dp)) {
                        Text("Reset")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { innerPadding ->
        if (!cameraGranted) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text(
                    text = "Camera permission required",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Button(
                    onClick = { permissionLauncher.launch(Manifest.permission.CAMERA) },
                    modifier = Modifier.padding(top = 16.dp)
                ) {
                    Text("Grant Camera")
                }
            }
            return@Scaffold
        }

        val state by viewModel.uiState.collectAsStateWithLifecycle()
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
        ) {
            IrCameraPreview(
                onFrame = viewModel::analyzeFrame,
                onCameraLensChanged = { cameraLens = it },
                modifier = Modifier.fillMaxSize()
            )
            Column(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .fillMaxWidth()
                    .background(Color.Black.copy(alpha = 0.58f))
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                val statusText = when {
                    state.roomTooBright -> "Too much ambient light. Dark room required."
                    cameraLens == IrCameraLens.BACK_FALLBACK -> "Front camera unavailable. IR scan may be unreliable."
                    cameraLens == IrCameraLens.UNAVAILABLE -> "No camera available."
                    cameraLens == IrCameraLens.FRONT -> "Front camera active. Dark room required."
                    else -> "Starting front camera. Dark room required."
                }
                Text(
                    text = statusText,
                    color = if (state.roomTooBright || cameraLens != IrCameraLens.FRONT) {
                        Color(0xFFFFEB3B)
                    } else {
                        Color.White
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Bold
                )
            }
            Canvas(modifier = Modifier.fillMaxSize()) {
                state.sources.forEach { source ->
                    val center = Offset(source.x * size.width, source.y * size.height)
                    val radius = 18.dp.toPx() + source.confidence * 14.dp.toPx()
                    drawCircle(
                        color = Color(0xFFFFEB3B).copy(alpha = 0.35f),
                        radius = radius,
                        center = center
                    )
                    drawCircle(
                        color = Color(0xFFFFEB3B),
                        radius = radius,
                        center = center,
                        style = Stroke(width = 3.dp.toPx())
                    )
                }
            }
            Row(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .background(Color.Black.copy(alpha = 0.58f))
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "${state.sources.size} source${if (state.sources.size == 1) "" else "s"}",
                    color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "Frames ${state.framesAnalyzed}  Peak ${state.peakCount}  Ambient ${state.ambientBrightness}",
                    color = Color.White.copy(alpha = 0.78f),
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}

private enum class IrCameraLens {
    STARTING,
    FRONT,
    BACK_FALLBACK,
    UNAVAILABLE
}

@Composable
private fun IrCameraPreview(
    onFrame: (Bitmap) -> Unit,
    onCameraLensChanged: (IrCameraLens) -> Unit,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val analysisExecutor = remember { Executors.newSingleThreadExecutor() }
    val cameraProviderFuture = remember { ProcessCameraProvider.getInstance(context) }

    DisposableEffect(Unit) {
        onDispose {
            runCatching { cameraProviderFuture.get().unbindAll() }
            analysisExecutor.shutdown()
        }
    }

    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            val previewView = PreviewView(ctx).apply {
                layoutParams = ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
                )
                scaleType = PreviewView.ScaleType.FILL_CENTER
                implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            }

            cameraProviderFuture.addListener({
                val cameraProvider = cameraProviderFuture.get()
                val preview = Preview.Builder()
                    .build()
                    .also { it.setSurfaceProvider(previewView.surfaceProvider) }
                val analysis = ImageAnalysis.Builder()
                    .setTargetResolution(Size(640, 480))
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .build()
                    .also {
                        it.setAnalyzer(analysisExecutor) { imageProxy ->
                            try {
                                imageProxy.toBitmapSafe()?.let(onFrame)
                            } finally {
                                imageProxy.close()
                            }
                        }
                    }
                val selector = runCatching {
                    when {
                        cameraProvider.hasCamera(CameraSelector.DEFAULT_FRONT_CAMERA) -> {
                            onCameraLensChanged(IrCameraLens.FRONT)
                            CameraSelector.DEFAULT_FRONT_CAMERA
                        }
                        cameraProvider.hasCamera(CameraSelector.DEFAULT_BACK_CAMERA) -> {
                            onCameraLensChanged(IrCameraLens.BACK_FALLBACK)
                            CameraSelector.DEFAULT_BACK_CAMERA
                        }
                        else -> {
                            onCameraLensChanged(IrCameraLens.UNAVAILABLE)
                            null
                        }
                    }
                }.getOrNull()
                if (selector == null) {
                    cameraProvider.unbindAll()
                    return@addListener
                }
                cameraProvider.unbindAll()
                cameraProvider.bindToLifecycle(lifecycleOwner, selector, preview, analysis)
            }, ContextCompat.getMainExecutor(ctx))

            previewView
        }
    )
}

private fun ImageProxy.toBitmapSafe(): Bitmap? {
    return try {
        if (planes.size < 3) return null
        val nv21 = toNv21()
        val yuvImage = YuvImage(nv21, ImageFormat.NV21, width, height, null)
        val out = ByteArrayOutputStream()
        yuvImage.compressToJpeg(Rect(0, 0, width, height), 80, out)
        val imageBytes = out.toByteArray()
        BitmapFactory.decodeByteArray(imageBytes, 0, imageBytes.size)
    } catch (_: Exception) {
        null
    }
}

private fun ImageProxy.toNv21(): ByteArray {
    val ySize = width * height
    val uvWidth = width / 2
    val uvHeight = height / 2
    val out = ByteArray(ySize + uvWidth * uvHeight * 2)

    copyPlaneToOutput(
        plane = planes[0],
        planeWidth = width,
        planeHeight = height,
        output = out,
        outputOffset = 0,
        outputPixelStride = 1
    )
    copyPlaneToOutput(
        plane = planes[2],
        planeWidth = uvWidth,
        planeHeight = uvHeight,
        output = out,
        outputOffset = ySize,
        outputPixelStride = 2
    )
    copyPlaneToOutput(
        plane = planes[1],
        planeWidth = uvWidth,
        planeHeight = uvHeight,
        output = out,
        outputOffset = ySize + 1,
        outputPixelStride = 2
    )

    return out
}

private fun copyPlaneToOutput(
    plane: ImageProxy.PlaneProxy,
    planeWidth: Int,
    planeHeight: Int,
    output: ByteArray,
    outputOffset: Int,
    outputPixelStride: Int
) {
    val buffer = plane.buffer.duplicate()
    val rowStride = plane.rowStride
    val pixelStride = plane.pixelStride
    val rowBuffer = ByteArray(rowStride)
    var outputIndex = outputOffset

    for (row in 0 until planeHeight) {
        val rowStart = row * rowStride
        val rowLength = if (pixelStride == 1 && outputPixelStride == 1) {
            planeWidth
        } else {
            (planeWidth - 1) * pixelStride + 1
        }
        buffer.position(rowStart)
        if (pixelStride == 1 && outputPixelStride == 1) {
            buffer.get(output, outputIndex, planeWidth)
            outputIndex += planeWidth
        } else {
            buffer.get(rowBuffer, 0, rowLength)
            var inputIndex = 0
            repeat(planeWidth) {
                output[outputIndex] = rowBuffer[inputIndex]
                outputIndex += outputPixelStride
                inputIndex += pixelStride
            }
        }
    }
}
