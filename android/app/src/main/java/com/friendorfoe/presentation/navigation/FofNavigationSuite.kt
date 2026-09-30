package com.friendorfoe.presentation.navigation

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.animation.animateColorAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController

@Composable
fun FofNavigationSuite(
    showNavigation: Boolean,
    currentRoute: String?,
    onNavigate: (TopLevelDestination) -> Unit,
    content: @Composable (PaddingValues) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val useRail = maxWidth >= 600.dp && maxWidth > maxHeight
        Scaffold(
            bottomBar = {
                if (showNavigation && !useRail) {
                    CompactNavigationBar(currentRoute, onNavigate)
                }
            },
        ) { scaffoldPadding ->
            Row(Modifier.fillMaxSize().padding(scaffoldPadding).consumeWindowInsets(scaffoldPadding)) {
                if (showNavigation && useRail) {
                    NavigationRail(
                        Modifier.testTag("navigation_rail").width(112.dp).verticalScroll(rememberScrollState()),
                        containerColor = MaterialTheme.colorScheme.surface,
                    ) {
                        primaryDestinations.forEach { destination ->
                            NavigationRailItem(
                                alwaysShowLabel = true,
                                selected = currentRoute == destination.route,
                                onClick = { onNavigate(destination) },
                                icon = { Icon(destination.icon, contentDescription = null) },
                                label = { DestinationLabel(destination) },
                                modifier = destinationModifier(destination),
                            )
                        }
                    }
                }
                Box(Modifier.weight(1f).fillMaxHeight()) { content(PaddingValues()) }
            }
        }
    }
}

@Composable
private fun CompactNavigationBar(currentRoute: String?, onNavigate: (TopLevelDestination) -> Unit) {
    val measurer = rememberTextMeasurer()
    val density = LocalDensity.current
    val labelStyle = MaterialTheme.typography.labelSmall
    // Allocate space to complete labels, rather than squeezing every label into
    // the same narrow slot. Large system text remains at the user's chosen size.
    val widths = primaryDestinations.map { destination ->
        with(density) { measurer.measure(destination.label, labelStyle).size.width.toDp() }
            .plus(8.dp).coerceAtLeast(56.dp)
    }
    Surface(color = MaterialTheme.colorScheme.surface) {
        Row(Modifier.fillMaxWidth().testTag("navigation_bar").navigationBarsPadding().selectableGroup()) {
            primaryDestinations.forEachIndexed { index, destination ->
                val selected = destination.route == currentRoute
                val indicator by animateColorAsState(
                    if (selected) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surface,
                    label = "tab indicator",
                )
                Column(
                    Modifier.weight(widths[index].value).heightIn(min = 80.dp)
                        .testTag("nav_destination")
                        .selectable(selected = selected, role = Role.Tab, onClick = { onNavigate(destination) })
                        .semantics(mergeDescendants = true) { contentDescription = destination.label }
                        .padding(vertical = 10.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Box(Modifier.background(indicator, RoundedCornerShape(24.dp)).padding(horizontal = 16.dp, vertical = 4.dp)) {
                        Icon(destination.icon, contentDescription = null, modifier = Modifier.size(24.dp),
                            tint = if (selected) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    DestinationLabel(destination)
                }
            }
        }
    }
}

@Composable
private fun DestinationLabel(destination: TopLevelDestination) {
    Text(
        destination.label,
        Modifier.clearAndSetSemantics {},
        style = MaterialTheme.typography.labelSmall,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

private fun destinationModifier(destination: TopLevelDestination): Modifier = Modifier
    .testTag("nav_destination")
    .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
    .semantics(mergeDescendants = true) { contentDescription = destination.label }

fun navigateTopLevel(navController: NavHostController, destination: TopLevelDestination) {
    navController.navigate(destination.route) {
        popUpTo("main_graph") { saveState = true }
        launchSingleTop = true
        restoreState = true
    }
}
