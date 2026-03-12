package com.friendorfoe.presentation

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.friendorfoe.presentation.navigation.FriendOrFoeNavGraph
import com.friendorfoe.presentation.navigation.Screen
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import dagger.hilt.android.AndroidEntryPoint

/**
 * Main entry point for the Friend or Foe app.
 *
 * Sets up edge-to-edge display, Compose theming, and the navigation
 * scaffold with bottom navigation bar.
 */
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
fun FriendOrFoeApp() {
    val navController = rememberNavController()
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = navBackStackEntry?.destination

    // Hide bottom nav on detail screen
    val showBottomBar = currentDestination?.route != Screen.Detail.route

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        bottomBar = {
            if (showBottomBar) {
                NavigationBar {
                    val bottomNavItems = listOf(
                        BottomNavItem("AR View", Screen.ArView.route),
                        BottomNavItem("List", Screen.ListView.route),
                        BottomNavItem("History", Screen.History.route)
                    )

                    bottomNavItems.forEach { item ->
                        NavigationBarItem(
                            selected = currentDestination?.hierarchy?.any {
                                it.route == item.route
                            } == true,
                            onClick = {
                                navController.navigate(item.route) {
                                    popUpTo(navController.graph.findStartDestination().id) {
                                        saveState = true
                                    }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            },
                            icon = { /* TODO: Add icons */ },
                            label = { Text(item.label) }
                        )
                    }
                }
            }
        }
    ) { innerPadding ->
        Box(modifier = Modifier.padding(innerPadding)) {
            FriendOrFoeNavGraph(navController = navController)
        }
    }
}

private data class BottomNavItem(
    val label: String,
    val route: String
)
