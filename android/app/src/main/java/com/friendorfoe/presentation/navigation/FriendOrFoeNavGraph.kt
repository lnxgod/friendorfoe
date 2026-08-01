package com.friendorfoe.presentation.navigation

import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavGraphBuilder
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.navigation
import androidx.navigation.navArgument
import com.friendorfoe.data.preferences.sanitizeTopLevelRoute
import com.friendorfoe.presentation.about.AboutLandingActions
import com.friendorfoe.presentation.about.AboutLandingScreen
import com.friendorfoe.presentation.about.AboutViewModel
import com.friendorfoe.presentation.about.InfoSettingsScreen
import com.friendorfoe.presentation.about.openUri
import com.friendorfoe.presentation.aircraft.AircraftReferenceScreen
import com.friendorfoe.presentation.ar.ArViewModel
import com.friendorfoe.presentation.ar.ArViewScreen
import com.friendorfoe.presentation.ar.PermissionHandler
import com.friendorfoe.presentation.badge.BadgeControlScreen
import com.friendorfoe.presentation.calibrate.CalibrateScreen
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import com.friendorfoe.presentation.detail.DetailScreen
import com.friendorfoe.presentation.detail.HistoricalDetailScreen
import com.friendorfoe.presentation.drones.DroneReferenceScreen
import com.friendorfoe.presentation.history.HistoryScreen
import com.friendorfoe.presentation.list.ListViewScreen
import com.friendorfoe.presentation.map.MapViewScreen
import com.friendorfoe.presentation.privacy.EmfSweepScreen
import com.friendorfoe.presentation.privacy.IrCameraScanScreen
import com.friendorfoe.presentation.privacy.IgnoredDevicesScreen
import com.friendorfoe.presentation.privacy.PrivacyScreen
import com.friendorfoe.presentation.privacy.PrivacyFindingDetailsRoute
import com.friendorfoe.presentation.reference.ReferenceGuideScreen
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.ContextualPermissionGate
import com.friendorfoe.presentation.permissions.FeaturePermissionGate
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.rememberPermissionBindings

private const val MAIN_GRAPH_ROUTE = "main_graph"
private const val REFERENCE_GUIDE_BASE_ROUTE = "reference_guide"

@Composable
fun MainNavGraph(
    navController: NavHostController,
    startRoute: String,
    modifier: Modifier = Modifier,
) {
    NavHost(
        navController = navController,
        startDestination = MAIN_GRAPH_ROUTE,
        modifier = modifier,
        enterTransition = { fadeIn(tween(300)) + slideInHorizontally { it / 4 } },
        exitTransition = { fadeOut(tween(200)) },
        popEnterTransition = { fadeIn(tween(300)) + slideInHorizontally { -it / 4 } },
        popExitTransition = { fadeOut(tween(200)) + slideOutHorizontally { it / 4 } },
    ) {
        navigation(
            route = MAIN_GRAPH_ROUTE,
            startDestination = sanitizeTopLevelRoute(startRoute),
        ) {
            registerSevenTopLevelDestinations(navController)
            registerSecondaryDestinations(navController)
        }
    }
}

@Composable
internal fun TopLevelRouteRoot(
    destination: TopLevelDestination,
    content: @Composable () -> Unit,
) {
    Box(
        Modifier.fillMaxSize().testTag("screen_${destination.label.lowercase()}"),
    ) { content() }
}

@Composable
internal fun ArPermissionRoute(
    cameraState: PermissionUiState,
    locationState: PermissionUiState,
    onRequestCamera: () -> Unit,
    onOpenCameraSettings: () -> Unit,
    content: @Composable (PermissionUiState) -> Unit,
) {
    FeaturePermissionGate(
        feature = AppFeature.AR_CAMERA,
        state = cameraState,
        onRequest = onRequestCamera,
        onOpenSettings = onOpenCameraSettings,
    ) {
        content(locationState)
    }
}

private fun NavGraphBuilder.registerSevenTopLevelDestinations(
    navController: NavHostController,
) {
    composable(Screen.ArView.route) {
        TopLevelRouteRoot(TopLevelDestination.AR) {
            val arViewModel: ArViewModel = hiltViewModel()
            val permissions = rememberPermissionBindings()
            ArPermissionRoute(
                cameraState = permissions.stateFor(AppFeature.AR_CAMERA),
                locationState = permissions.stateFor(AppFeature.AR_MAP_LOCATION),
                onRequestCamera = { permissions.request(AppFeature.AR_CAMERA) },
                onOpenCameraSettings = { permissions.openSettings(AppFeature.AR_CAMERA) },
            ) { locationState ->
                PermissionHandler(
                    viewModel = arViewModel,
                    locationPermissionState = locationState,
                    onRequestLocation = { permissions.request(AppFeature.AR_MAP_LOCATION) },
                    onOpenLocationSettings = {
                        permissions.openSettings(AppFeature.AR_MAP_LOCATION)
                    },
                ) {
                    ArViewScreen(
                        onObjectTapped = { objectId ->
                            navController.navigate(Screen.Detail.createRoute(objectId))
                        },
                        viewModel = arViewModel,
                        isPreciseLocation = locationState == PermissionUiState.Granted,
                    )
                }
            }
        }
    }

    composable(Screen.MapView.route) {
        TopLevelRouteRoot(TopLevelDestination.MAP) {
            val permissions = rememberPermissionBindings()
            val locationState = permissions.stateFor(AppFeature.AR_MAP_LOCATION)
            MapViewScreen(
                onObjectTapped = { objectId ->
                    navController.navigate(Screen.Detail.createRoute(objectId))
                },
                locationPermissionState = locationState,
                onRequestLocation = {
                    permissions.request(AppFeature.AR_MAP_LOCATION)
                },
                onOpenLocationSettings = {
                    permissions.openSettings(AppFeature.AR_MAP_LOCATION)
                },
            )
        }
    }

    composable(Screen.ListView.route) {
        TopLevelRouteRoot(TopLevelDestination.LIST) {
            ListViewScreen(
                onObjectTapped = { objectId ->
                    navController.navigate(Screen.Detail.createRoute(objectId))
                },
                onNavigateToReferenceGuide = {
                    navController.navigate(REFERENCE_GUIDE_BASE_ROUTE) { launchSingleTop = true }
                },
                onNavigateToAbout = {
                    navigateTopLevel(navController, TopLevelDestination.ABOUT)
                },
            )
        }
    }

    composable(Screen.Privacy.route) {
        TopLevelRouteRoot(TopLevelDestination.PRIVACY) {
            PrivacyScreen(
                onOpenIgnoredDevices = {
                    navController.navigate(Screen.IgnoredDevices.route) { launchSingleTop = true }
                },
                onOpenInfo = {
                    navigateTopLevel(navController, TopLevelDestination.ABOUT)
                },
                onOpenFinding = { key ->
                    navController.navigate(Screen.PrivacyFinding.createRoute(key))
                },
            )
        }
    }

    composable(Screen.Badge.route) {
        TopLevelRouteRoot(TopLevelDestination.BADGE) {
            BadgeControlScreen()
        }
    }

    composable(Screen.History.route) {
        TopLevelRouteRoot(TopLevelDestination.HISTORY) {
            HistoryScreen(
                onEntryTapped = { historyId ->
                    navController.navigate(Screen.HistoricalDetail.createRoute(historyId))
                },
                onNavigateToReferenceGuide = {
                    navController.navigate(REFERENCE_GUIDE_BASE_ROUTE) { launchSingleTop = true }
                },
                onNavigateToAbout = {
                    navigateTopLevel(navController, TopLevelDestination.ABOUT)
                },
            )
        }
    }

    composable(Screen.About.route) {
        AboutTopLevelRoute(navController = navController)
    }
}

@Composable
internal fun AboutTopLevelRoute(
    navController: NavHostController,
) {
    val context = LocalContext.current
    TopLevelRouteRoot(TopLevelDestination.ABOUT) {
        AboutLandingScreen(
            actions = AboutLandingActions(
                onOpenSettings = {
                    navController.navigate(Screen.AboutSettings.route) { launchSingleTop = true }
                },
                onOpenReference = {
                    navController.navigate(REFERENCE_GUIDE_BASE_ROUTE) { launchSingleTop = true }
                },
                onContactSupport = {
                    context.openUri("mailto:lnxgod@gmail.com?subject=Friend%20or%20Foe%20feedback")
                },
                onOpenGithub = {
                    context.openUri("https://github.com/lnxgod/friendorfoe")
                },
            ),
        )
    }
}

private fun NavGraphBuilder.registerSecondaryDestinations(
    navController: NavHostController,
) {
    composable(Screen.AboutSettings.route) {
        Column(Modifier.fillMaxSize()) {
            FofSecondaryScreenHeader(
                title = "App settings",
                onBack = navController::popBackStack,
            )
            InfoSettingsScreen(
                viewModel = hiltViewModel<AboutViewModel>(),
                onNavigateToCalibrate = {
                    navController.navigate(Screen.Calibrate.route) { launchSingleTop = true }
                },
                onNavigateToEmfSweep = {
                    navController.navigate(Screen.EmfSweep.route) { launchSingleTop = true }
                },
                onNavigateToIrCameraScan = {
                    navController.navigate(Screen.IrCameraScan.route) { launchSingleTop = true }
                },
                onNavigateToReference = {
                    navController.navigate(REFERENCE_GUIDE_BASE_ROUTE) { launchSingleTop = true }
                },
                modifier = Modifier.weight(1f),
            )
        }
    }

    composable(
        route = Screen.Detail.route,
        arguments = listOf(navArgument("objectId") { type = NavType.StringType }),
    ) { backStackEntry ->
        val objectId = backStackEntry.arguments?.getString("objectId") ?: return@composable
        DetailScreen(
            objectId = objectId,
            onBack = { navController.popBackStack() },
            onNavigateToDroneGuide = { manufacturer ->
                navController.navigate(Screen.DroneGuide.createRoute(manufacturer))
            },
            onNavigateToAircraftGuide = { typeCode ->
                navController.navigate(Screen.AircraftGuide.createRoute(typeCode))
            },
        )
    }

    composable(
        route = Screen.HistoricalDetail.route,
        arguments = listOf(navArgument("historyId") { type = NavType.LongType }),
    ) { backStackEntry ->
        HistoricalDetailScreen(
            historyId = requireNotNull(backStackEntry.arguments).getLong("historyId"),
            onBack = navController::popBackStack,
            onReturnToHistory = {
                navController.popBackStack(Screen.History.route, inclusive = false)
            },
        )
    }

    composable(
        route = Screen.ReferenceGuide.route,
        arguments = listOf(
            navArgument("tab") {
                type = NavType.StringType
                defaultValue = ""
                nullable = true
            },
            navArgument("query") {
                type = NavType.StringType
                defaultValue = ""
                nullable = true
            },
        ),
    ) {
        ReferenceGuideScreen(onBack = { navController.popBackStack() })
    }

    composable(
        route = Screen.DroneGuide.route,
        arguments = listOf(
            navArgument("manufacturer") {
                type = NavType.StringType
                defaultValue = ""
                nullable = true
            },
        ),
    ) { backStackEntry ->
        val manufacturer = backStackEntry.arguments?.getString("manufacturer")
            ?.takeIf { it.isNotBlank() }
        DroneReferenceScreen(
            onBack = { navController.popBackStack() },
            initialManufacturerFilter = manufacturer,
        )
    }

    composable(
        route = Screen.AircraftGuide.route,
        arguments = listOf(
            navArgument("type") {
                type = NavType.StringType
                defaultValue = ""
                nullable = true
            },
        ),
    ) { backStackEntry ->
        val typeCode = backStackEntry.arguments?.getString("type")
            ?.takeIf { it.isNotBlank() }
        AircraftReferenceScreen(
            onBack = { navController.popBackStack() },
            initialTypeFilter = typeCode,
        )
    }

    composable(Screen.EmfSweep.route) {
        EmfSweepScreen(onBack = { navController.popBackStack() })
    }

    composable(Screen.IrCameraScan.route) {
        val permissions = rememberPermissionBindings()
        ContextualPermissionGate(
            feature = AppFeature.IR_CAMERA,
            bindings = permissions,
            onBack = { navController.popBackStack() },
        ) {
            IrCameraScanScreen(onBack = { navController.popBackStack() })
        }
    }

    composable(Screen.IgnoredDevices.route) {
        IgnoredDevicesScreen(onBack = { navController.popBackStack() })
    }

    composable(
        route = Screen.PrivacyFinding.route,
        arguments = listOf(
            navArgument("source") { type = NavType.StringType },
            navArgument("record") { type = NavType.StringType },
        ),
    ) {
        PrivacyFindingDetailsRoute(
            onBack = { navController.popBackStack() },
            onBackToPrivacy = {
                navigateTopLevel(navController, TopLevelDestination.PRIVACY)
            },
        )
    }

    composable(
        route = Screen.BadgeFocus.route,
        arguments = listOf(navArgument("focusKey") { type = NavType.StringType }),
    ) { backStackEntry ->
        val focusKey = backStackEntry.arguments?.getString("focusKey")
        BadgeControlScreen(initialFocusKey = focusKey)
    }

    composable(Screen.Calibrate.route) {
        CalibrateScreen(onBack = { navController.popBackStack() })
    }
}
