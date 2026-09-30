package com.friendorfoe.presentation

import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.navigation.NavType
import androidx.navigation.compose.*
import androidx.navigation.navArgument
import com.friendorfoe.presentation.navigation.Screen
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class SkyAlertNavigationTest {
    @get:Rule val compose = createComposeRule()

    @Test fun notificationOpensExactObjectOnceIncludingEncodedCharacters() {
        val objectId = "remote:drone/with spaces%42"
        var pending by mutableStateOf<String?>(objectId)
        var consumed = 0
        var reached: String? = null
        compose.setContent {
            val nav = rememberNavController()
            val entry by nav.currentBackStackEntryAsState()
            PendingSkyObjectNavigationEffect(nav, entry?.destination?.route, pending) {
                consumed += 1
                pending = null
            }
            NavHost(nav, startDestination = Screen.About.route) {
                composable(Screen.About.route) { Text("More") }
                composable(Screen.Detail.route, arguments = listOf(navArgument("objectId") { type = NavType.StringType })) {
                    reached = it.arguments?.getString("objectId")
                    Text("Selected object: $reached")
                }
            }
        }
        compose.onNodeWithText("Selected object: $objectId").assertIsDisplayed()
        compose.runOnIdle {
            assertEquals(objectId, reached)
            assertEquals(1, consumed)
            assertEquals(null, pending)
        }
    }
}
