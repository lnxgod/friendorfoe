package com.friendorfoe.presentation

import android.app.Activity
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.sanitizeTopLevelRoute
import com.friendorfoe.presentation.alerts.SkyAlertMonitorViewModel
import com.friendorfoe.presentation.components.FofLaunchPlaceholder
import com.friendorfoe.presentation.navigation.BackDisposition
import com.friendorfoe.presentation.navigation.FofNavigationSuite
import com.friendorfoe.presentation.navigation.MainNavGraph
import com.friendorfoe.presentation.navigation.TopLevelDestination
import com.friendorfoe.presentation.navigation.backDisposition
import com.friendorfoe.presentation.navigation.navigateTopLevel
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.welcome.WelcomeScreen
import dagger.hilt.android.AndroidEntryPoint

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            FriendOrFoeTheme {
                FriendOrFoeApp()
            }
        }
    }
}

@Composable
fun FriendOrFoeApp(viewModel: AppChromeViewModel = hiltViewModel()) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    SkyAlertMonitor()
    when (val launch = state.launchState) {
        AppLaunchState.Loading -> FofLaunchPlaceholder()
        AppLaunchState.NeedsOnboarding -> WelcomeScreen(viewModel::completeOnboarding)
        is AppLaunchState.Ready -> MainApplicationShell(
            startRoute = launch.startRoute,
            onTopLevelSelected = viewModel::recordTopLevelRoute,
        )
    }
}

@Composable
fun MainApplicationShell(
    startRoute: String,
    onTopLevelSelected: (String) -> Unit,
    onExitRequested: (() -> Unit)? = null,
) {
    val navController = rememberNavController()
    val graphStartRoute = rememberSaveable { sanitizeTopLevelRoute(startRoute) }
    val entry by navController.currentBackStackEntryAsState()
    val currentRoute = entry?.destination?.route
    val isTopLevel = currentRoute in TopLevelDestination.entries.map { it.route }
    val activity = LocalContext.current as? Activity

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
