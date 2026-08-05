package com.friendorfoe.presentation

import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.friendorfoe.presentation.navigation.Screen
import com.friendorfoe.presentation.privacy.PendingPrivacyRouteQueue
import com.friendorfoe.presentation.privacy.PrivacyFindingKey
import com.friendorfoe.presentation.privacy.PrivacyNotificationIdStore
import com.friendorfoe.presentation.privacy.PrivacyNotificationRoute
import com.friendorfoe.presentation.privacy.PrivacySourceKind
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class PrivacyLaunchNavigationTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun validNotificationOverridesAboutWithExactArgumentsAndIsConsumedOnce() {
        val expectedKey = PrivacyFindingKey(
            source = PrivacySourceKind.PHONE_BLE,
            sourceRecordId = "observation:pairing spam/42",
        )
        val notification = PrivacyNotificationRoute.from(expectedKey, SequentialIdStore())
        val queue = PendingPrivacyRouteQueue()
        val consumptionResults = mutableListOf<Boolean>()
        var reachedKey: PrivacyFindingKey? = null
        assertTrue(queue.offer(notification.dataUri, notification.route))

        setNavigationHarness(
            queue = queue,
            onConsumed = { route -> consumptionResults += queue.consume(route) },
            onFindingReached = { reachedKey = it },
        )

        compose.onNodeWithTag("privacy_finding_destination").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals(expectedKey, reachedKey)
            assertEquals(listOf(true), consumptionResults)
            assertNull(queue.pending.value)
        }
    }

    @Test
    fun invalidNotificationInputRemainsOnAboutAndIsNeverConsumed() {
        val queue = PendingPrivacyRouteQueue()
        val consumedRoutes = mutableListOf<String>()
        assertFalse(
            queue.offer(
                dataUri = "friendorfoe://privacy/finding/unknown/entity%3A42",
                routeExtra = "privacy/finding/unknown/entity%3A42",
            ),
        )

        setNavigationHarness(
            queue = queue,
            onConsumed = consumedRoutes::add,
            onFindingReached = {},
        )

        compose.onNodeWithTag("about_destination").assertIsDisplayed()
        compose.runOnIdle {
            assertTrue(consumedRoutes.isEmpty())
            assertNull(queue.pending.value)
        }
    }

    private fun setNavigationHarness(
        queue: PendingPrivacyRouteQueue,
        onConsumed: (String) -> Unit,
        onFindingReached: (PrivacyFindingKey) -> Unit,
    ) {
        compose.setContent {
            FriendOrFoeTheme {
                val navController = rememberNavController()
                val entry by navController.currentBackStackEntryAsState()
                val pendingRoute by queue.pending.collectAsStateWithLifecycle()
                PendingPrivacyRouteNavigationEffect(
                    navController = navController,
                    currentRoute = entry?.destination?.route,
                    pendingPrivacyRoute = pendingRoute,
                    onPrivacyRouteConsumed = onConsumed,
                )
                NavHost(navController = navController, startDestination = Screen.About.route) {
                    composable(Screen.About.route) {
                        Text("About", Modifier.testTag("about_destination"))
                    }
                    composable(
                        route = Screen.PrivacyFinding.route,
                        arguments = listOf(
                            navArgument("source") { type = NavType.StringType },
                            navArgument("record") { type = NavType.StringType },
                        ),
                    ) { backStackEntry ->
                        val key = Screen.PrivacyFinding.keyFromNavigationArguments(
                            source = backStackEntry.arguments?.getString("source"),
                            record = backStackEntry.arguments?.getString("record"),
                        )
                        if (key != null) onFindingReached(key)
                        Text("Finding", Modifier.testTag("privacy_finding_destination"))
                    }
                }
            }
        }
    }

    private class SequentialIdStore : PrivacyNotificationIdStore {
        override fun idFor(key: PrivacyFindingKey): Int = 1
    }
}
