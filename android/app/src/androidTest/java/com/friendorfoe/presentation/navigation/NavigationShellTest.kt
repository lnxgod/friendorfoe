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
import androidx.compose.ui.test.performScrollTo
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navigation
import androidx.test.espresso.Espresso.pressBack
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.repository.AppUpdateMetadata
import com.friendorfoe.presentation.about.InfoUiState
import com.friendorfoe.presentation.about.UpdateUiState
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionBindings
import com.friendorfoe.presentation.permissions.PermissionSettingsLaunchResult
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertSame
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

        assertEquals("info", Screen.About.route)
        assertEquals("info/settings", Screen.AboutSettings.route)

        listOf("AR", "Map", "List", "Privacy", "Badge", "History", "About")
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
                            composable(Screen.About.route) {
                                TopLevelRouteRoot(TopLevelDestination.ABOUT) {
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

        compose.onNodeWithContentDescription("About").performClick()
        compose.onNodeWithText("Open ignored").performClick()
        compose.onNodeWithTag("screen_ignored_devices").assertIsDisplayed()
        compose.onNodeWithTag("navigation_bar").assertDoesNotExist()

        pressBack()
        compose.onNodeWithTag("screen_about").assertIsDisplayed()

        compose.runOnUiThread { persistedStart.value = Screen.Badge.route }
        compose.waitForIdle()

        compose.onNodeWithTag("screen_about").assertIsDisplayed()
        assertEquals(0, hostDisposals.intValue)
    }

    @Test
    fun productionAboutReferenceActionNavigatesThroughARealNavHost() {
        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                NavHost(navController = navController, startDestination = "test_about") {
                    composable("test_about") {
                        AboutTopLevelRoute(navController = navController)
                    }
                    composable("reference_guide") {
                        Text("Reference guide", Modifier.testTag("screen_reference_guide"))
                    }
                }
            }
        }

        compose.onNodeWithTag("about_reference").performClick()

        compose.onNodeWithTag("screen_reference_guide").assertIsDisplayed()
    }

    @Test
    fun productionAboutSettingsRouteReturnsToAboutOnBack() {
        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                NavHost(navController = navController, startDestination = Screen.About.route) {
                    composable(Screen.About.route) {
                        AboutTopLevelRoute(navController = navController)
                    }
                    composable(Screen.AboutSettings.route) {
                        AboutSettingsRoute(
                            navController = navController,
                            viewModel = null,
                            permissionBindings = PermissionBindings(
                                states = AppFeature.entries.associateWith {
                                    PermissionUiState.Granted
                                },
                                requestFeature = {},
                                openFeatureSettings = { _, _ ->
                                    PermissionSettingsLaunchResult.Opened
                                },
                            ),
                        )
                    }
                }
            }
        }

        compose.onNodeWithTag("about_app_settings").performClick()
        compose.onNodeWithTag("screen_about_settings").assertIsDisplayed()

        pressBack()

        compose.onNodeWithTag("about_landing").assertIsDisplayed()
    }

    @Test
    fun productionAboutUpdateActionUsesTheReleaseUrl() {
        var opened: String? = null
        val remote = AppUpdateMetadata(
            version = AppVersion(null, "0.68.0"),
            releaseUrl = "https://github.com/lnxgod/friendorfoe/releases/tag/v0.68.0",
        )

        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                AboutTopLevelRoute(
                    navController = navController,
                    state = InfoUiState(
                        installedVersion = AppVersion(120, "0.67.7"),
                        updateState = UpdateUiState.Available(remote),
                    ),
                    onOpenUpdate = { opened = it },
                )
            }
        }

        compose.onNodeWithTag("about_open_update").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(remote.releaseUrl, opened) }
    }

    @Test
    fun productionAboutRouteStartsIdleCheckOncePerCompositionEntry() {
        var checks = 0
        val installedName = mutableStateOf("0.67.7")

        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                AboutTopLevelRoute(
                    navController = navController,
                    state = InfoUiState(
                        installedVersion = AppVersion(120, installedName.value),
                    ),
                    onCheckForUpdatesIfIdle = { checks++ },
                )
            }
        }

        compose.waitForIdle()
        compose.runOnIdle { assertEquals(1, checks) }
        compose.runOnUiThread { installedName.value = "0.67.8" }
        compose.waitForIdle()
        compose.runOnIdle { assertEquals(1, checks) }
    }

    @Test
    fun aboutAndSettingsResolveTheSameMainGraphScopeOwner() {
        var aboutOwner: Any? = null
        var settingsOwner: Any? = null

        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                NavHost(navController, startDestination = "main_graph") {
                    navigation(
                        route = "main_graph",
                        startDestination = Screen.About.route,
                    ) {
                        composable(Screen.About.route) {
                            aboutOwner = mainGraphAboutOwner(navController)
                            Button(onClick = { navController.navigate(Screen.AboutSettings.route) }) {
                                Text("Open settings owner")
                            }
                        }
                        composable(Screen.AboutSettings.route) {
                            settingsOwner = mainGraphAboutOwner(navController)
                            Text("Settings owner", Modifier.testTag("test_settings_owner"))
                        }
                    }
                }
            }
        }

        compose.onNodeWithText("Open settings owner").performClick()
        compose.onNodeWithTag("test_settings_owner").assertIsDisplayed()
        compose.runOnIdle {
            assertNotNull(aboutOwner)
            assertSame(aboutOwner, settingsOwner)
        }
    }
}
