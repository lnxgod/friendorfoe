package com.friendorfoe.presentation.privacy

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageFormat
import android.graphics.PointF
import android.graphics.Rect
import android.graphics.YuvImage
import android.util.Size
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.UseCase
import androidx.camera.core.UseCaseGroup
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.camera.view.TransformExperimental
import androidx.camera.view.transform.CoordinateTransform
import androidx.camera.view.transform.ImageProxyTransformFactory
import androidx.camera.view.transform.OutputTransform
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import com.friendorfoe.detection.IrCameraDetector
import java.io.ByteArrayOutputStream
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

interface IrCameraBinder {
    fun bind(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        onFrame: (IrPreviewFrame) -> Unit,
        onFailure: (Throwable) -> Unit,
    )

    fun unbind()
}

internal class IrBindingGenerationGate {
    private val activeToken = AtomicLong(0L)

    fun beginGeneration(): Long = activeToken.incrementAndGet()

    fun invalidate() {
        activeToken.incrementAndGet()
    }

    fun isActive(generation: Long): Boolean = activeToken.get() == generation

    fun claimFailure(generation: Long): Boolean =
        activeToken.compareAndSet(generation, generation + 1L)

    fun isClaimedFailureCurrent(generation: Long): Boolean =
        activeToken.get() == generation + 1L
}

internal fun requireConvertedFrame(bitmap: Bitmap?): Bitmap =
    bitmap ?: throw IllegalStateException("Could not analyze camera frame")

/** Route-scoped CameraX owner. It never unbinds camera use cases owned by another screen. */
class CameraXIrCameraBinder(
    context: Context,
    private val detector: IrCameraDetector = IrCameraDetector(),
) : IrCameraBinder {
    private data class BindingResources(
        val generation: Long,
        val executor: ExecutorService,
        var provider: ProcessCameraProvider? = null,
        var analysis: ImageAnalysis? = null,
        var useCases: List<UseCase> = emptyList(),
    )

    private val applicationContext = context.applicationContext
    private val mainExecutor = ContextCompat.getMainExecutor(applicationContext)
    private val generationGate = IrBindingGenerationGate()
    private var activeResources: BindingResources? = null

    override fun bind(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        onFrame: (IrPreviewFrame) -> Unit,
        onFailure: (Throwable) -> Unit,
    ) {
        unbind()
        val generation = generationGate.beginGeneration()
        val analysisSession = detector.newSession()
        val frameExecutor = Executors.newSingleThreadExecutor()
        val resources = BindingResources(
            generation = generation,
            executor = frameExecutor,
        )
        activeResources = resources
        val providerFuture = ProcessCameraProvider.getInstance(applicationContext)
        providerFuture.addListener(
            {
                if (!generationGate.isActive(generation)) return@addListener
                val provider = runCatching { providerFuture.get() }.getOrElse { error ->
                    reportFailure(generation, error, onFailure)
                    return@addListener
                }
                resources.provider = provider

                val selectorAndFacing = selectCamera(provider).getOrElse { error ->
                    reportFailure(generation, error, onFailure)
                    return@addListener
                }
                val (selector, frontCamera) = selectorAndFacing
                val preview = Preview.Builder().build().also {
                    it.setSurfaceProvider(previewView.surfaceProvider)
                }
                val analysis = ImageAnalysis.Builder()
                    .setResolutionSelector(
                        ResolutionSelector.Builder()
                            .setResolutionStrategy(
                                ResolutionStrategy(
                                    Size(640, 480),
                                    ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER,
                                ),
                            )
                            .build(),
                    )
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .build()
                resources.analysis = analysis
                analysis.setAnalyzer(frameExecutor) { imageProxy ->
                    analyzeFrame(
                        generation = generation,
                        analysisSession = analysisSession,
                        imageProxy = imageProxy,
                        previewView = previewView,
                        frontCamera = frontCamera,
                        onFrame = onFrame,
                        onFailure = onFailure,
                    )
                }

                val useCases = listOf<UseCase>(preview, analysis)
                resources.useCases = useCases
                val group = UseCaseGroup.Builder()
                    .addUseCase(preview)
                    .addUseCase(analysis)
                    .apply { previewView.viewPort?.let(::setViewPort) }
                    .build()
                runCatching {
                    provider.bindToLifecycle(lifecycleOwner, selector, group)
                }.onSuccess {
                    if (!generationGate.isActive(generation) || activeResources !== resources) {
                        provider.unbind(*useCases.toTypedArray())
                    }
                }.onFailure { error ->
                    reportFailure(generation, error, onFailure)
                }
            },
            mainExecutor,
        )
    }

    override fun unbind() {
        generationGate.invalidate()
        val resources = activeResources
        activeResources = null
        resources?.let(::cleanupResources)
    }

    private fun selectCamera(
        provider: ProcessCameraProvider,
    ): Result<Pair<CameraSelector, Boolean>> = runCatching {
        when {
            provider.hasCamera(CameraSelector.DEFAULT_FRONT_CAMERA) -> {
                CameraSelector.DEFAULT_FRONT_CAMERA to true
            }
            provider.hasCamera(CameraSelector.DEFAULT_BACK_CAMERA) -> {
                CameraSelector.DEFAULT_BACK_CAMERA to false
            }
            else -> error("No camera is available on this device")
        }
    }

    @TransformExperimental
    private fun analyzeFrame(
        generation: Long,
        analysisSession: IrCameraDetector.Session,
        imageProxy: ImageProxy,
        previewView: PreviewView,
        frontCamera: Boolean,
        onFrame: (IrPreviewFrame) -> Unit,
        onFailure: (Throwable) -> Unit,
    ) {
        try {
            if (!generationGate.isActive(generation)) return
            val sourceTransform = runCatching {
                ImageProxyTransformFactory().apply {
                    setUsingCropRect(true)
                    setUsingRotationDegrees(true)
                }.getOutputTransform(imageProxy)
            }.getOrNull()
            val crop = imageProxy.cropRect
            val metadata = AnalysisFrameMetadata(
                imageWidth = imageProxy.width,
                imageHeight = imageProxy.height,
                crop = IntRect(crop.left, crop.top, crop.right, crop.bottom),
                rotationDegrees = imageProxy.imageInfo.rotationDegrees,
                frontCamera = frontCamera,
            )
            val bitmap = requireConvertedFrame(imageProxy.toBitmapSafe())
            val frameAnalysis = try {
                analysisSession.analyzeFrameWithEnvironment(bitmap)
            } finally {
                bitmap.recycle()
            }
            mainExecutor.execute {
                if (!generationGate.isActive(generation)) return@execute
                val previewWidth = previewView.width.toFloat()
                val previewHeight = previewView.height.toFloat()
                if (previewWidth <= 0f || previewHeight <= 0f) return@execute
                val mappedCenters = mapWithCameraX(
                    sourceTransform = sourceTransform,
                    targetTransform = previewView.outputTransform,
                    centers = frameAnalysis.sources.map { it.centerPx },
                )
                onFrame(
                    IrPreviewFrame(
                        analysis = frameAnalysis,
                        metadata = metadata,
                        previewWidthPx = previewWidth,
                        previewHeightPx = previewHeight,
                        mappedCentersPx = mappedCenters,
                    ),
                )
            }
        } catch (error: Throwable) {
            reportFailure(generation, error, onFailure)
        } finally {
            imageProxy.close()
        }
    }

    @TransformExperimental
    private fun mapWithCameraX(
        sourceTransform: OutputTransform?,
        targetTransform: OutputTransform?,
        centers: List<FloatPoint>,
    ): List<FloatPoint>? {
        if (sourceTransform == null || targetTransform == null) return null
        return runCatching {
            val transform = CoordinateTransform(sourceTransform, targetTransform)
            centers.map { center ->
                val point = PointF(center.x, center.y)
                transform.mapPoint(point)
                FloatPoint(point.x, point.y)
            }
        }.getOrNull()
    }

    private fun reportFailure(
        generation: Long,
        error: Throwable,
        onFailure: (Throwable) -> Unit,
    ) {
        if (!generationGate.claimFailure(generation)) return
        mainExecutor.execute {
            if (!generationGate.isClaimedFailureCurrent(generation)) return@execute
            val resources = activeResources
                ?.takeIf { it.generation == generation }
            if (resources != null) {
                activeResources = null
                cleanupResources(resources)
            }
            onFailure(error)
        }
    }

    private fun cleanupResources(resources: BindingResources) {
        resources.analysis?.clearAnalyzer()
        if (resources.useCases.isNotEmpty()) {
            runCatching {
                resources.provider?.unbind(*resources.useCases.toTypedArray())
            }
        }
        resources.executor.shutdownNow()
    }
}

private fun ImageProxy.toBitmapSafe(): Bitmap? {
    if (planes.size < 3) return null
    return try {
        val nv21 = toNv21()
        val yuvImage = YuvImage(nv21, ImageFormat.NV21, width, height, null)
        val output = ByteArrayOutputStream()
        yuvImage.compressToJpeg(Rect(0, 0, width, height), 82, output)
        val bytes = output.toByteArray()
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
    } catch (_: RuntimeException) {
        null
    }
}

private fun ImageProxy.toNv21(): ByteArray {
    val ySize = width * height
    val uvWidth = width / 2
    val uvHeight = height / 2
    val output = ByteArray(ySize + uvWidth * uvHeight * 2)

    copyPlaneToOutput(planes[0], width, height, output, 0, 1)
    copyPlaneToOutput(planes[2], uvWidth, uvHeight, output, ySize, 2)
    copyPlaneToOutput(planes[1], uvWidth, uvHeight, output, ySize + 1, 2)
    return output
}

private fun copyPlaneToOutput(
    plane: ImageProxy.PlaneProxy,
    planeWidth: Int,
    planeHeight: Int,
    output: ByteArray,
    outputOffset: Int,
    outputPixelStride: Int,
) {
    val buffer = plane.buffer.duplicate()
    val rowStride = plane.rowStride
    val pixelStride = plane.pixelStride
    val rowBuffer = ByteArray(rowStride)
    var outputIndex = outputOffset

    repeat(planeHeight) { row ->
        val rowLength = if (pixelStride == 1 && outputPixelStride == 1) {
            planeWidth
        } else {
            (planeWidth - 1) * pixelStride + 1
        }
        buffer.position(row * rowStride)
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
