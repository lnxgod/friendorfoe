package com.friendorfoe.presentation.navigation

import androidx.compose.runtime.Composable
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.navArgument
import androidx.hilt.navigation.compose.hiltViewModel
import com.friendorfoe.presentation.about.AboutScreen
import com.friendorfoe.presentation.ar.ArViewModel
import com.friendorfoe.presentation.ar.ArViewScreen
import com.friendorfoe.presentation.ar.PermissionHandler
import com.friendorfoe.presentation.detail.DetailScreen
import com.friendorfoe.presentation.history.HistoryScreen
import com.friendorfoe.presentation.list.ListViewScreen
import com.friendorfoe.presentation.map.MapViewScreen

/**
 * Main navigation graph for the app.
 *
 * Defines routes and composable destinations for all screens.
 */
@Composable
fun FriendOrFoeNavGraph(
    navController: NavHostController
) {
    NavHost(
        navController = navController,
        startDestination = Screen.ArView.route
    ) {
        composable(Screen.ArView.route) {
            val arViewModel: ArViewModel = hiltViewModel()
            PermissionHandler(viewModel = arViewModel) {
                ArViewScreen(
                    onObjectTapped = { objectId ->
                        navController.navigate(Screen.Detail.createRoute(objectId))
                    },
                    viewModel = arViewModel
                )
            }
        }

        composable(Screen.MapView.route) {
            MapViewScreen(
                onObjectTapped = { objectId ->
                    navController.navigate(Screen.Detail.createRoute(objectId))
                }
            )
        }

        composable(Screen.ListView.route) {
            ListViewScreen(
                onObjectTapped = { objectId ->
                    navController.navigate(Screen.Detail.createRoute(objectId))
                }
            )
        }

        composable(Screen.History.route) {
            HistoryScreen(
                onEntryTapped = { objectId ->
                    navController.navigate(Screen.Detail.createRoute(objectId))
                }
            )
        }

        composable(Screen.About.route) {
            AboutScreen(onBack = { navController.popBackStack() })
        }

        composable(
            route = Screen.Detail.route,
            arguments = listOf(
                navArgument("objectId") { type = NavType.StringType }
            )
        ) { backStackEntry ->
            val objectId = backStackEntry.arguments?.getString("objectId") ?: return@composable
            DetailScreen(
                objectId = objectId,
                onBack = { navController.popBackStack() }
            )
        }
    }
}
