package com.friendorfoe.presentation.reference

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.components.FofEmptyState

@Composable
internal fun ReferenceNoMatches(kind: String, onReset: () -> Unit) {
    Column(Modifier.fillMaxSize().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center) {
        FofEmptyState(title = "No matching $kind", detail = "Try another name or clear the filters.")
        TextButton(onClick = onReset, modifier = Modifier.testTag("reference_reset")) { Text("Reset search") }
    }
}

@Composable
internal fun ReferenceExpansionHint(expanded: Boolean) {
    Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(if (expanded) "Hide details" else "View details", Modifier.weight(1f),
            style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
        Icon(if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
            contentDescription = null, tint = MaterialTheme.colorScheme.primary)
    }
}
