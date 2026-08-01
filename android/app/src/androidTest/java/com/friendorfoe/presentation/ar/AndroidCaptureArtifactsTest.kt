package com.friendorfoe.presentation.ar

import android.content.ContentValues
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AndroidCaptureArtifactsTest {
    @Test
    fun jpegCaptureNormalizes90And270DegreeFramesExactlyOnce() {
        val source = asymmetricRedBlueJpeg(width = 80, height = 40)

        val clockwise = normalizeJpegCapture(
            jpegBytes = source,
            sourceWidth = 80,
            sourceHeight = 40,
            rotationDegrees = 90,
        )
        val counterClockwise = normalizeJpegCapture(
            jpegBytes = source,
            sourceWidth = 80,
            sourceHeight = 40,
            rotationDegrees = 270,
        )

        assertEquals(40, clockwise.widthPx)
        assertEquals(80, clockwise.heightPx)
        assertEquals(40, counterClockwise.widthPx)
        assertEquals(80, counterClockwise.heightPx)

        val clockwiseBitmap = decode(clockwise)
        assertRedDominant(clockwiseBitmap.getPixel(20, 10))
        assertBlueDominant(clockwiseBitmap.getPixel(20, 70))

        val counterClockwiseBitmap = decode(counterClockwise)
        assertBlueDominant(counterClockwiseBitmap.getPixel(20, 10))
        assertRedDominant(counterClockwiseBitmap.getPixel(20, 70))
    }

    @Test
    fun yuvCaptureNormalizesRotatedDimensionsAndPixelOrientation() {
        val width = 80
        val height = 40
        val nv21 = asymmetricDarkLightNv21(width, height)

        val payload = capturePayloadFromNv21(
            nv21 = nv21,
            sourceWidth = width,
            sourceHeight = height,
            rotationDegrees = 90,
        )

        assertEquals(height, payload.widthPx)
        assertEquals(width, payload.heightPx)
        val bitmap = decode(payload)
        val top = Color.red(bitmap.getPixel(bitmap.width / 2, 10))
        val bottom = Color.red(bitmap.getPixel(bitmap.width / 2, bitmap.height - 10))
        assertTrue("Expected the dark source half at the top after one 90° rotation", top < bottom)
    }

    @Test
    fun legacyWriterOmitsRelativePath() {
        val sink = RecordingMediaStoreSink()
        val writer = AndroidPhotoWriter(sink, apiLevel = 28)

        val result = runSuspend { writer.write(captureDraft("friendorfoe_legacy.jpg")) }

        assertTrue(result.isSuccess)
        assertFalse(
            sink.insertedValues.single().containsKey(MediaStore.Images.Media.RELATIVE_PATH),
        )
    }

    @Test
    fun modernWriterUsesFriendOrFoePicturesPath() {
        val sink = RecordingMediaStoreSink()
        val writer = AndroidPhotoWriter(sink, apiLevel = 29)

        val result = runSuspend { writer.write(captureDraft("friendorfoe_modern.jpg")) }

        assertTrue(result.isSuccess)
        assertEquals(
            "${Environment.DIRECTORY_PICTURES}/FriendOrFoe",
            sink.insertedValues.single().getAsString(MediaStore.Images.Media.RELATIVE_PATH),
        )
    }

    @Test
    fun explicitSaveCreatesExactlyOneCompleteRow() {
        val sink = RecordingMediaStoreSink()
        val writer = AndroidPhotoWriter(sink)
        val draft = captureDraft("friendorfoe_explicit_save.jpg")

        val result = runSuspend { writer.write(draft) }

        assertTrue(result.isSuccess)
        assertEquals(1, sink.insertedValues.size)
        assertEquals(setOf(sink.insertedUri), sink.survivingRows)
        assertArrayEquals(draft.payload.bytes, sink.output.toByteArray())
        assertEquals(SavedPhoto(sink.insertedUri.toString()), result.getOrNull())
    }

    @Test
    fun partialWriteDeletesTheExactInsertedRow() {
        val sink = RecordingMediaStoreSink(failAfterBytes = 2)
        val writer = AndroidPhotoWriter(sink)

        val result = runSuspend { writer.write(captureDraft("friendorfoe_partial.jpg")) }

        assertTrue(result.isFailure)
        assertEquals(1, sink.insertedValues.size)
        assertEquals(listOf(sink.insertedUri), sink.deletedUris)
        assertEquals(emptySet<Uri>(), sink.survivingRows)
    }

    @Test
    fun cancelledBlockingWriteDeletesItsExactRowBeforeTheNextWriteStarts() = runBlocking {
        val sink = BlockingMediaStoreSink()
        val firstWriter = AndroidPhotoWriter(sink, apiLevel = 35)
        val secondWriter = AndroidPhotoWriter(sink, apiLevel = 35)
        val firstUri = Uri.parse("content://friendorfoe/images/1")
        val secondUri = Uri.parse("content://friendorfoe/images/2")

        val first = async(Dispatchers.Default) {
            firstWriter.write(captureDraft("friendorfoe_blocked.jpg"))
        }
        assertTrue(sink.firstWriteStarted.await(5, TimeUnit.SECONDS))

        first.cancel()
        val second = async(Dispatchers.Default) {
            secondWriter.write(captureDraft("friendorfoe_after_cancel.jpg"))
        }
        delay(100)

        assertEquals(1, sink.insertedUris.size)
        assertFalse(second.isCompleted)

        sink.releaseFirstWrite.countDown()
        try {
            first.await()
        } catch (_: CancellationException) {
            // The writer must preserve structured cancellation after cleaning up its row.
        }
        assertTrue(second.await().isSuccess)

        assertEquals(1, sink.maximumConcurrentWrites.get())
        assertEquals(listOf(firstUri), sink.deletedUris)
        assertEquals(setOf(secondUri), sink.survivingRows)
    }

    @Test
    fun shareCreatesReadableCacheUriAndNoGalleryRow() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val uniqueName = "friendorfoe_share_only_${System.nanoTime()}.jpg"
        val draft = captureDraft(uniqueName)
        val before = galleryRowsNamed(uniqueName)

        val result = runSuspend { AndroidShareImageFactory(context).create(draft) }

        val request = result.getOrThrow()
        assertEquals("image/jpeg", request.mimeType)
        val bytes = context.contentResolver.openInputStream(Uri.parse(request.contentUri))!!.use {
            it.readBytes()
        }
        assertArrayEquals(draft.payload.bytes, bytes)
        assertEquals(before, galleryRowsNamed(uniqueName))
    }

    @Test
    fun shareConsumerGrantsTemporaryReadAccess() {
        val request = ShareRequest("content://friendorfoe.fileprovider/shared/test.jpg", "image/jpeg")

        val intent = captureShareIntent(request)

        assertEquals(Intent.ACTION_SEND, intent.action)
        assertEquals("image/jpeg", intent.type)
        assertEquals(Uri.parse(request.contentUri), intent.getParcelableExtra(Intent.EXTRA_STREAM))
        assertTrue(intent.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION != 0)
    }

    private fun galleryRowsNamed(displayName: String): Int {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        return context.contentResolver.query(
            MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
            arrayOf(MediaStore.Images.Media._ID),
            "${MediaStore.Images.Media.DISPLAY_NAME} = ?",
            arrayOf(displayName),
            null,
        )?.use { it.count } ?: 0
    }
}

private fun asymmetricRedBlueJpeg(width: Int, height: Int): ByteArray {
    val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    for (y in 0 until height) {
        for (x in 0 until width) {
            bitmap.setPixel(x, y, if (x < width / 2) Color.RED else Color.BLUE)
        }
    }
    return ByteArrayOutputStream().use { output ->
        check(bitmap.compress(Bitmap.CompressFormat.JPEG, 100, output))
        bitmap.recycle()
        output.toByteArray()
    }
}

private fun asymmetricDarkLightNv21(width: Int, height: Int): ByteArray {
    val bytes = ByteArray(width * height * 3 / 2)
    for (y in 0 until height) {
        for (x in 0 until width) {
            bytes[y * width + x] = if (x < width / 2) 40 else 220.toByte()
        }
    }
    for (index in width * height until bytes.size) bytes[index] = 128.toByte()
    return bytes
}

private fun decode(payload: CapturePayload): Bitmap =
    checkNotNull(BitmapFactory.decodeByteArray(payload.bytes, 0, payload.bytes.size))

private fun assertRedDominant(pixel: Int) {
    assertTrue(Color.red(pixel) > Color.blue(pixel) + 80)
}

private fun assertBlueDominant(pixel: Int) {
    assertTrue(Color.blue(pixel) > Color.red(pixel) + 80)
}

private class BlockingMediaStoreSink : MediaStoreSink {
    val firstWriteStarted = CountDownLatch(1)
    val releaseFirstWrite = CountDownLatch(1)
    val maximumConcurrentWrites = AtomicInteger(0)
    val insertedUris = mutableListOf<Uri>()
    val deletedUris = mutableListOf<Uri>()
    val survivingRows = mutableSetOf<Uri>()
    private val activeWrites = AtomicInteger(0)

    override fun insert(values: ContentValues): Uri = synchronized(this) {
        Uri.parse("content://friendorfoe/images/${insertedUris.size + 1}").also { uri ->
            insertedUris += uri
            survivingRows += uri
        }
    }

    override fun openOutputStream(uri: Uri): OutputStream = object : OutputStream() {
        override fun write(value: Int) = Unit

        override fun write(bytes: ByteArray, offset: Int, length: Int) {
            val active = activeWrites.incrementAndGet()
            maximumConcurrentWrites.updateAndGet { previous -> maxOf(previous, active) }
            try {
                if (uri == Uri.parse("content://friendorfoe/images/1")) {
                    firstWriteStarted.countDown()
                    check(releaseFirstWrite.await(5, TimeUnit.SECONDS)) {
                        "Timed out waiting to release the first write"
                    }
                }
            } finally {
                activeWrites.decrementAndGet()
            }
        }
    }

    override fun delete(uri: Uri): Int = synchronized(this) {
        deletedUris += uri
        if (survivingRows.remove(uri)) 1 else 0
    }
}

private class RecordingMediaStoreSink(
    private val failAfterBytes: Int? = null,
) : MediaStoreSink {
    val insertedUri: Uri = Uri.parse("content://friendorfoe/images/41")
    val insertedValues = mutableListOf<ContentValues>()
    val deletedUris = mutableListOf<Uri>()
    val survivingRows = mutableSetOf<Uri>()
    val output = ByteArrayOutputStream()

    override fun insert(values: ContentValues): Uri {
        insertedValues += ContentValues(values)
        survivingRows += insertedUri
        return insertedUri
    }

    override fun openOutputStream(uri: Uri): OutputStream {
        check(uri == insertedUri)
        val failureBoundary = failAfterBytes ?: return output
        return object : OutputStream() {
            private var written = 0

            override fun write(value: Int) {
                if (written >= failureBoundary) throw IllegalStateException("simulated write failure")
                output.write(value)
                written += 1
            }
        }
    }

    override fun delete(uri: Uri): Int {
        deletedUris += uri
        return if (survivingRows.remove(uri)) 1 else 0
    }
}

private fun captureDraft(displayName: String) = CaptureDraft(
    payload = CapturePayload(
        bytes = byteArrayOf(10, 20, 30, 40),
        mimeType = "image/jpeg",
        widthPx = 4,
        heightPx = 3,
    ),
    displayName = displayName,
    description = "Visual capture",
)

private fun <T> runSuspend(block: suspend () -> T): T = kotlinx.coroutines.runBlocking { block() }
