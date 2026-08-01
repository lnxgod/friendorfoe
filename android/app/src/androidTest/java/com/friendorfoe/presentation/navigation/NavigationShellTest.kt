package com.friendorfoe.presentation.navigation

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navigation
import androidx.test.espresso.Espresso.pressBack
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class NavigationShellTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun sevenDestinationsAreReachableWithoutHiltScreenDependencies() {
        val selected = mutableStateOf(TopLevelDestination.AR)
        compose.setContent {
            FriendOrFoeTheme {
                FofNavigationSuite(
                    showNavigation = true,
                    currentRoute = selected.value.route,
                    onNavigate = { selected.value = it },
                ) { padding ->
                    TopLevelRouteRoot(selected.value) {
                        Text(selected.value.label, Modifier.padding(padding))
                    }
                }
            }
        }

        listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info")
            .forEach { label ->
                compose.onNodeWithContentDescription(label).assertHasClickAction().performClick()
                compose.onNodeWithTag("screen_${label.lowercase()}").assertIsDisplayed()
            }
    }

    @Test
    fun secondaryBackRestoresTopLevelWithoutRecreatingHostOrChangingFrozenStart() {
        val persistedStart = mutableStateOf(Screen.ArView.route)
        val hostDisposals = mutableIntStateOf(0)

        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                val frozenStart = rememberSaveable { persistedStart.value }
                val entry by navController.currentBackStackEntryAsState()
                val currentRoute = entry?.destination?.route

                DisposableEffect(navController) {
                    onDispose { hostDisposals.intValue++ }
                }

                FofNavigationSuite(
                    showNavigation = currentRoute in TopLevelDestination.entries.map { it.route },
                    currentRoute = currentRoute,
                    onNavigate = { destination ->
                        navigateTopLevel(navController, destination)
                    },
                ) { padding ->
                    NavHost(
                        navController = navController,
                        startDestination = "main_graph",
                        modifier = Modifier.padding(padding).testTag("stable_host"),
                    ) {
                        navigation(route = "main_graph", startDestination = frozenStart) {
                            composable(Screen.ArView.route) {
                                TopLevelRouteRoot(TopLevelDestination.AR) { Text("AR") }
                            }
                            composable(Screen.Info.route) {
                                TopLevelRouteRoot(TopLevelDestination.INFO) {
                                    Button(onClick = { navController.navigate(Screen.IgnoredDevices.route) }) {
                                        Text("Open ignored")
                                    }
                                }
                            }
                            composable(Screen.IgnoredDevices.route) {
                                Text("Ignored devices", Modifier.testTag("screen_ignored_devices"))
                            }
                        }
                    }
                }
            }
        }

        compose.onNodeWithContentDescription("Info").performClick()
        compose.onNodeWithText("Open ignored").performClick()
        compose.onNodeWithTag("screen_ignored_devices").assertIsDisplayed()
        compose.onNodeWithTag("navigation_bar").assertDoesNotExist()

        pressBack()
        compose.onNodeWithTag("screen_info").assertIsDisplayed()

        compose.runOnUiThread { persistedStart.value = Screen.Badge.route }
        compose.waitForIdle()

        compose.onNodeWithTag("screen_info").assertIsDisplayed()
        assertEquals(0, hostDisposals.intValue)
    }
}
