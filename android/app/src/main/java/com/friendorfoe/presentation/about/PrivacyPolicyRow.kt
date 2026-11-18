package com.friendorfoe.presentation.about

import android.util.Xml
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.friendorfoe.R
import com.friendorfoe.presentation.components.FofActionRow
import org.xmlpull.v1.XmlPullParser
import java.io.InputStream

// The same HTML can be published as a static page. Read only its article
// elements, so the on-device reader uses native text, links and font scaling.
private data class PolicyBlock(val tag: String, val text: String, val href: String?)

private fun readPrivacyPolicy(input: InputStream): List<PolicyBlock> {
    val xml = Xml.newPullParser().apply { setInput(input, "UTF-8") }
    return buildList {
        while (xml.next() != XmlPullParser.END_DOCUMENT) {
            if (xml.eventType != XmlPullParser.START_TAG) continue
            val tag = xml.name
            if (tag in setOf("h1", "h2", "p", "a")) {
                val href = xml.getAttributeValue(null, "href")
                add(PolicyBlock(tag, xml.nextText(), href))
            }
        }
    }
}

@Composable
internal fun PrivacyPolicyRow() {
    var open by rememberSaveable { mutableStateOf(false) }
    FofActionRow(
        title = "Privacy policy",
        description = "Read how Friend or Foe uses your data",
        onClick = { open = true },
        modifier = Modifier.testTag("open_privacy_policy"),
    )
    if (!open) return
    val context = LocalContext.current
    val document = remember(context) {
        context.resources.openRawResource(R.raw.privacy_policy).use(::readPrivacyPolicy)
    }
    AlertDialog(
        onDismissRequest = { open = false },
        title = { Text(document.first { it.tag == "h1" }.text) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                for (entry in document) {
                    when (entry.tag) {
                        "h2" -> Text(entry.text, style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.padding(top = 16.dp).semantics { heading() })
                        "p" -> SelectionContainer {
                            Text(entry.text, modifier = Modifier.padding(vertical = 6.dp),
                                style = MaterialTheme.typography.bodyMedium)
                        }
                        "a" -> entry.href?.let { href ->
                            TextButton(
                                onClick = { context.openUri(href) },
                                modifier = Modifier.fillMaxWidth(),
                            ) { Text(entry.text) }
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { open = false }) { Text("Close") } },
    )
}
