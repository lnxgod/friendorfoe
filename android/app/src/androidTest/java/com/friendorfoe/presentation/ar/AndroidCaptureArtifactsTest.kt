package com.friendorfoe.presentation.ar

import android.content.ContentValues
import android.content.Intent
import android.net.Uri
import android.provider.MediaStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AndroidCaptureArtifactsTest {
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
