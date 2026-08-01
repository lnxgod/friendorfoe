package com.friendorfoe.presentation

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.sanitizeTopLevelRoute
import com.friendorfoe.data.repository.RuntimePermissionChangeNotifier
import com.friendorfoe.detection.WifiScanCoordinator
import com.friendorfoe.presentation.alerts.SkyAlertMonitorViewModel
import com.friendorfoe.presentation.components.FofLaunchPlaceholder
import com.friendorfoe.presentation.navigation.BackDisposition
import com.friendorfoe.presentation.navigation.FofNavigationSuite
import com.friendorfoe.presentation.navigation.MainNavGraph
import com.friendorfoe.presentation.navigation.TopLevelDestination
import com.friendorfoe.presentation.navigation.backDisposition
import com.friendorfoe.presentation.navigation.navigateTopLevel
import com.friendorfoe.presentation.privacy.PendingPrivacyRouteQueue
import com.friendorfoe.presentation.privacy.PrivacyLaunchIntentPayload
import com.friendorfoe.presentation.privacy.PrivacyNotificationRoute
import com.friendorfoe.presentation.privacy.acceptPrivacyLaunchIntent
import com.friendorfoe.presentation.permissions.PermissionStateRepository
import com.friendorfoe.presentation.permissions.capturePermissionRationales
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.welcome.WelcomeScreen
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import kotlinx.coroutines.launch

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    @Inject lateinit var permissionStateRepository: PermissionStateRepository
    @Inject lateinit var runtimePermissionChangeNotifier: RuntimePermissionChangeNotifier
    @Inject lateinit var wifiScanCoordinator: WifiScanCoordinator

    private lateinit var pendingPrivacyRoute: PendingPrivacyRouteQueue

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pendingPrivacyRoute = PendingPrivacyRouteQueue(
            savedInstanceState?.getString(SAVED_PRIVACY_ROUTE),
        )
        enqueuePrivacyRoute(intent)
        enableEdgeToEdge()
        setContent {
            val pendingRoute by pendingPrivacyRoute.pending.collectAsStateWithLifecycle()
            FriendOrFoeTheme {
                FriendOrFoeApp(
                    pendingPrivacyRoute = pendingRoute,
                    onPrivacyRouteConsumed = pendingPrivacyRoute::consume,
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        enqueuePrivacyRoute(intent)
    }

    override fun onResume() {
        super.onResume()
        // Activity-backed rationale evidence is captured before any coroutine can suspend.
        val rationales = capturePermissionRationales(this)
        notifyRuntimePlatformStateChanged(
            runtimePermissionChangeNotifier,
            wifiScanCoordinator,
        )
        lifecycleScope.launch {
            permissionStateRepository.refresh(rationales)
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        pendingPrivacyRoute.savedRoute()?.let { outState.putString(SAVED_PRIVACY_ROUTE, it) }
        super.onSaveInstanceState(outState)
    }

    private fun enqueuePrivacyRoute(intent: Intent?) {
        if (intent == null) return
        val remaining = acceptPrivacyLaunchIntent(
            queue = pendingPrivacyRoute,
            payload = PrivacyLaunchIntentPayload(
                dataUri = intent.dataString,
                routeExtra = intent.getStringExtra(PrivacyNotificationRoute.EXTRA_ROUTE),
            ),
        )
        if (remaining.dataUri == null && remaining.routeExtra == null) {
            intent.data = null
            intent.removeExtra(PrivacyNotificationRoute.EXTRA_ROUTE)
        }
    }

    private companion object {
        const val SAVED_PRIVACY_ROUTE = "pending_privacy_finding_route"
    }
}

internal fun notifyRuntimePlatformStateChanged(
    runtimePermissionChangeNotifier: RuntimePermissionChangeNotifier,
    wifiScanCoordinator: WifiScanCoordinator,
) {
    runtimePermissionChangeNotifier.onRuntimePermissionsChanged()
    wifiScanCoordinator.notifyPlatformStateChanged()
}

@Composable
fun FriendOrFoeApp(
    viewModel: AppChromeViewModel = hiltViewModel(),
    pendingPrivacyRoute: String? = null,
    onPrivacyRouteConsumed: (String) -> Unit = {},
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    SkyAlertMonitor()
    when (val launch = state.launchState) {
        AppLaunchState.Loading -> FofLaunchPlaceholder()
        AppLaunchState.NeedsOnboarding -> WelcomeScreen(viewModel::completeOnboarding)
        is AppLaunchState.Ready -> MainApplicationShell(
            startRoute = launch.startRoute,
            onTopLevelSelected = viewModel::recordTopLevelRoute,
            pendingPrivacyRoute = pendingPrivacyRoute,
            onPrivacyRouteConsumed = onPrivacyRouteConsumed,
        )
    }
}

@Composable
fun MainApplicationShell(
    startRoute: String,
    onTopLevelSelected: (String) -> Unit,
    onExitRequested: (() -> Unit)? = null,
    pendingPrivacyRoute: String? = null,
    onPrivacyRouteConsumed: (String) -> Unit = {},
) {
    val navController = rememberNavController()
    val graphStartRoute = rememberSaveable { sanitizeTopLevelRoute(startRoute) }
    val entry by navController.currentBackStackEntryAsState()
    val currentRoute = entry?.destination?.route
    val isTopLevel = currentRoute in TopLevelDestination.entries.map { it.route }
    val activity = LocalContext.current as? Activity

    LaunchedEffect(pendingPrivacyRoute, currentRoute) {
        val route = pendingPrivacyRoute ?: return@LaunchedEffect
        if (currentRoute == null) return@LaunchedEffect
        navController.navigate(route) { launchSingleTop = true }
        onPrivacyRouteConsumed(route)
    }

    BackHandler(enabled = currentRoute != null) {
        when (backDisposition(currentRoute)) {
            BackDisposition.EXIT_APP -> {
                if (onExitRequested != null) onExitRequested() else activity?.finish()
            }
            BackDisposition.POP_SECONDARY -> navController.popBackStack()
        }
    }

    FofNavigationSuite(
        showNavigation = isTopLevel,
        currentRoute = currentRoute,
        onNavigate = { destination ->
            navigateTopLevel(navController, destination)
            onTopLevelSelected(destination.route)
        },
    ) { padding ->
        MainNavGraph(navController, graphStartRoute, Modifier.padding(padding))
    }
}

@Composable
private fun SkyAlertMonitor() {
    hiltViewModel<SkyAlertMonitorViewModel>()
}
