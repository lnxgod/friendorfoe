package com.friendorfoe.presentation.ar

import android.content.ContentResolver
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.OutputStream
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withContext

interface MediaStoreSink {
    fun insert(values: ContentValues): Uri?
    fun openOutputStream(uri: Uri): OutputStream?
    fun delete(uri: Uri): Int
}

class AndroidPhotoWriter internal constructor(
    private val sink: MediaStoreSink,
    private val apiLevel: Int = Build.VERSION.SDK_INT,
) : PhotoWriter {
    @Inject
    constructor(@ApplicationContext context: Context) : this(
        ContentResolverMediaStoreSink(context.contentResolver),
        Build.VERSION.SDK_INT,
    )

    private companion object {
        val processWriteMutex = Mutex()
    }

    override suspend fun write(draft: CaptureDraft): Result<SavedPhoto> {
        processWriteMutex.lock()
        return try {
            writeOwned(draft)
        } finally {
            processWriteMutex.unlock()
        }
    }

    private suspend fun writeOwned(draft: CaptureDraft): Result<SavedPhoto> {
        var insertedUri: Uri? = null
        var committed = false
        return try {
            val savedPhoto = withContext(Dispatchers.IO) {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, draft.displayName)
                    put(MediaStore.Images.Media.MIME_TYPE, draft.payload.mimeType)
                    if (apiLevel >= Build.VERSION_CODES.Q) {
                        put(
                            MediaStore.Images.Media.RELATIVE_PATH,
                            "${Environment.DIRECTORY_PICTURES}/FriendOrFoe",
                        )
                    }
                    put(MediaStore.Images.Media.DESCRIPTION, draft.description)
                }
                val uri = sink.insert(values)
                    ?: throw IllegalStateException("Could not create photo row")
                insertedUri = uri
                val stream = sink.openOutputStream(uri)
                    ?: throw IllegalStateException("Could not open photo row")
                stream.use {
                    it.write(draft.payload.bytes)
                    it.flush()
                }
                SavedPhoto(uri.toString())
            }
            committed = true
            Result.success(savedPhoto)
        } catch (cancellation: CancellationException) {
            throw cancellation
        } catch (failure: Throwable) {
            Result.failure(failure)
        } finally {
            if (!committed) {
                insertedUri?.let { uri ->
                    withContext(NonCancellable + Dispatchers.IO) {
                        runCatching { sink.delete(uri) }
                    }
                }
            }
        }
    }
}

private class ContentResolverMediaStoreSink(
    private val resolver: ContentResolver,
) : MediaStoreSink {
    override fun insert(values: ContentValues): Uri? =
        resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)

    override fun openOutputStream(uri: Uri): OutputStream? = resolver.openOutputStream(uri)

    override fun delete(uri: Uri): Int = resolver.delete(uri, null, null)
}
