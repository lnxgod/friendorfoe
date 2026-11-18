package com.friendorfoe.presentation.components

import android.content.res.AssetManager
import android.widget.Toast
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import com.google.gson.JsonParser
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private data class PhotoCredit(
    val title: String, val creator: String, val changes: String,
    val source: String, val license: String, val licenseUrl: String,
)

private object PhotoCredits {
    private var entries: Map<String, PhotoCredit>? = null

    @Synchronized
    fun get(assets: AssetManager, path: String): PhotoCredit? {
        val credits = entries ?: assets.open("verified-photos.json").bufferedReader().use { reader ->
            JsonParser.parseReader(reader).asJsonObject["photos"].asJsonArray.associate { item ->
                val row = item.asJsonObject
                row["path"].asString to PhotoCredit(row["title"].asString, row["creator"].asString,
                    row["changes"].asString, row["sourceURL"].asString, row["license"].asString, row["licenseURL"].asString)
            }
        }.also { entries = it }
        return credits[path]
    }
}

@Composable
internal fun ReferencePhotoCredit(path: String) {
    val context = LocalContext.current
    val assets = context.assets
    val uri = LocalUriHandler.current
    fun open(url: String) {
        try {
            uri.openUri(url)
        } catch (_: IllegalArgumentException) {
            Toast.makeText(context, "No browser is available to open this link.", Toast.LENGTH_SHORT).show()
        }
    }
    val credit by produceState<PhotoCredit?>(null, path) {
        value = withContext(Dispatchers.IO) { PhotoCredits.get(assets, path) }
    }
    credit?.let { photo ->
        Column {
            Text("Photo: ${photo.title} · ${photo.creator}", style = MaterialTheme.typography.bodySmall)
            Text(photo.changes, style = MaterialTheme.typography.bodySmall)
            Row {
                TextButton(onClick = { open(photo.source) }) { Text("Photo source") }
                TextButton(onClick = { open(photo.licenseUrl) }) { Text(photo.license) }
            }
        }
    }
}
