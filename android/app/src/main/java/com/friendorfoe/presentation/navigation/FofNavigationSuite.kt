package com.friendorfoe.presentation.navigation

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
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
        val showNavLabel = LocalDensity.current.fontScale < 1.3f
        Scaffold(
            bottomBar = {
                if (showNavigation && !useRail) {
                    NavigationBar(Modifier.testTag("navigation_bar")) {
                        TopLevelDestination.entries.forEach { destination ->
                            NavigationBarItem(
                                alwaysShowLabel = false,
                                selected = currentRoute == destination.route,
                                onClick = { onNavigate(destination) },
                                icon = { Icon(destination.icon, contentDescription = null) },
                                label = if (showNavLabel) {
                                    {
                                        Text(
                                            destination.label,
                                            Modifier.clearAndSetSemantics {},
                                            maxLines = 1,
                                            softWrap = false,
                                            overflow = TextOverflow.Clip,
                                        )
                                    }
                                } else null,
                                modifier = Modifier
                                    .testTag("nav_destination")
                                    .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
                                    .semantics(mergeDescendants = true) {
                                        contentDescription = destination.label
                                    },
                            )
                        }
                    }
                }
            },
        ) { scaffoldPadding ->
            Row(Modifier.fillMaxSize().padding(scaffoldPadding)) {
                if (showNavigation && useRail) {
                    NavigationRail(Modifier.testTag("navigation_rail")) {
                        TopLevelDestination.entries.forEach { destination ->
                            NavigationRailItem(
                                selected = currentRoute == destination.route,
                                onClick = { onNavigate(destination) },
                                icon = { Icon(destination.icon, contentDescription = null) },
                                label = if (showNavLabel) {
                                    {
                                        Text(
                                            destination.label,
                                            Modifier.clearAndSetSemantics {},
                                            maxLines = 1,
                                            softWrap = false,
                                            overflow = TextOverflow.Clip,
                                        )
                                    }
                                } else null,
                                modifier = Modifier
                                    .testTag("nav_destination")
                                    .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
                                    .semantics(mergeDescendants = true) {
                                        contentDescription = destination.label
                                    },
                            )
                        }
                    }
                }
                Box(Modifier.weight(1f).fillMaxHeight()) { content(PaddingValues()) }
            }
        }
    }
}

fun navigateTopLevel(
    navController: NavHostController,
    destination: TopLevelDestination,
) {
    navController.navigate(destination.route) {
        popUpTo("main_graph") {
            saveState = true
        }
        launchSingleTop = true
        restoreState = true
    }
}
