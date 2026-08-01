package com.friendorfoe.presentation.ar

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import com.friendorfoe.BuildConfig
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal fun captureShareIntent(request: ShareRequest): Intent =
    Intent(Intent.ACTION_SEND).apply {
        type = request.mimeType
        putExtra(Intent.EXTRA_STREAM, Uri.parse(request.contentUri))
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }

class AndroidShareImageFactory @Inject constructor(
    @ApplicationContext private val context: Context,
) : ShareImageFactory {
    override suspend fun create(draft: CaptureDraft): Result<ShareRequest> =
        withContext(Dispatchers.IO) {
            var cacheFile: File? = null
            try {
                val directory = File(context.cacheDir, "shared_captures").apply { mkdirs() }
                check(directory.isDirectory) { "Could not create share cache" }
                val baseName = draft.displayName.substringBeforeLast('.')
                    .replace(Regex("[^a-zA-Z0-9_-]"), "_")
                    .take(48)
                    .ifBlank { "capture" }
                val extension = when (draft.payload.mimeType) {
                    "image/png" -> ".png"
                    else -> ".jpg"
                }
                val file = File.createTempFile("${baseName}_", extension, directory)
                cacheFile = file
                file.outputStream().use {
                    it.write(draft.payload.bytes)
                    it.flush()
                }
                val uri = FileProvider.getUriForFile(
                    context,
                    "${BuildConfig.APPLICATION_ID}.fileprovider",
                    file,
                )
                Result.success(ShareRequest(uri.toString(), draft.payload.mimeType))
            } catch (cancellation: CancellationException) {
                cacheFile?.delete()
                throw cancellation
            } catch (failure: Throwable) {
                cacheFile?.delete()
                Result.failure(failure)
            }
        }
}
