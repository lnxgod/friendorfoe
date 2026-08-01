package com.friendorfoe.presentation.ar

import android.content.ContentResolver
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.OutputStream
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

interface MediaStoreSink {
    fun insert(values: ContentValues): Uri?
    fun openOutputStream(uri: Uri): OutputStream?
    fun delete(uri: Uri): Int
}

class AndroidPhotoWriter internal constructor(
    private val sink: MediaStoreSink,
) : PhotoWriter {
    @Inject
    constructor(@ApplicationContext context: Context) : this(
        ContentResolverMediaStoreSink(context.contentResolver),
    )

    override suspend fun write(draft: CaptureDraft): Result<SavedPhoto> = withContext(Dispatchers.IO) {
        var insertedUri: Uri? = null
        try {
            val values = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, draft.displayName)
                put(MediaStore.Images.Media.MIME_TYPE, draft.payload.mimeType)
                put(
                    MediaStore.Images.Media.RELATIVE_PATH,
                    "${Environment.DIRECTORY_PICTURES}/FriendOrFoe",
                )
                put(MediaStore.Images.Media.DESCRIPTION, draft.description)
            }
            val uri = sink.insert(values)
                ?: return@withContext Result.failure(IllegalStateException("Could not create photo row"))
            insertedUri = uri
            val stream = sink.openOutputStream(uri)
                ?: throw IllegalStateException("Could not open photo row")
            stream.use {
                it.write(draft.payload.bytes)
                it.flush()
            }
            Result.success(SavedPhoto(uri.toString()))
        } catch (cancellation: CancellationException) {
            insertedUri?.let(sink::delete)
            throw cancellation
        } catch (failure: Throwable) {
            insertedUri?.let(sink::delete)
            Result.failure(failure)
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
