package com.friendorfoe.presentation.welcome

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class WelcomeScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun primaryActionComesBeforeOptionalContentAndWelcomeHasNoUpdater() {
        var continued = 0
        setWelcome(onContinue = { continued++ })

        val action = compose.onNodeWithTag("welcome_get_started").assertIsDisplayed()
        val actionTop = action.fetchSemanticsNode().boundsInRoot.top
        val optionalTop = compose.onNodeWithTag("welcome_scope")
            .fetchSemanticsNode().boundsInRoot.top
        assertTrue(actionTop < optionalTop)
        action.performClick()
        compose.runOnIdle { assertEquals(1, continued) }

        listOf("Updates", "Check for Updates", "Update available").forEach { forbidden ->
            assertEquals(
                0,
                compose.onAllNodesWithText(forbidden, substring = true, ignoreCase = true)
                    .fetchSemanticsNodes().size,
            )
        }
    }

    @Test
    fun scopeDataAndPermissionTimingCopyStayTruthful() {
        setWelcome()

        listOf(
            "Observations are evidence, not proof of identity, intent, or ownership.",
            "Coverage depends on nearby signals, available data, granted permissions, and configured services.",
            "History may store observations and phone coordinates locally.",
            "Network features may exchange location or detection data with the service you use.",
            "Android asks for access when a feature needs it, not all at once during welcome.",
        ).forEach { fact ->
            compose.onNodeWithText(fact).performScrollTo().assertIsDisplayed()
        }
    }

    @Test
    fun optionalLinksOnlyOpenFixedHttpsDestinations() {
        val opened = mutableListOf<String>()
        setWelcome(onOpenLink = opened::add)

        compose.onNodeWithText("GameChangers")
            .performScrollTo()
            .performClick()
        compose.onNodeWithText("GitHub Repository")
            .performScrollTo()
            .performClick()

        compose.runOnIdle {
            assertEquals(
                listOf(
                    "https://gamechangersai.org",
                    "https://github.com/lnxgod/friendorfoe",
                ),
                opened,
            )
        }
    }

    @Test
    fun compactWelcomeKeepsPrimaryActionInBoundsAtOnePointThreeFontScale() {
        assertCompactWelcome(fontScale = 1.3f)
    }

    @Test
    fun compactWelcomeKeepsPrimaryActionInBoundsAtTwoTimesFontScale() {
        assertCompactWelcome(fontScale = 2f)
    }

    private fun assertCompactWelcome(fontScale: Float) {
        compose.setContent {
            val currentDensity = LocalDensity.current
            CompositionLocalProvider(
                LocalDensity provides Density(currentDensity.density, fontScale),
            ) {
                FriendOrFoeTheme {
                    Box(Modifier.width(360.dp).height(640.dp)) {
                        WelcomeContent(WelcomeActions())
                    }
                }
            }
        }

        val frame = compose.onNodeWithTag("welcome_scroll").fetchSemanticsNode().boundsInRoot
        val action = compose.onNodeWithTag("welcome_get_started")
            .assertIsDisplayed()
            .fetchSemanticsNode().boundsInRoot
        assertTrue(action.left >= frame.left)
        assertTrue(action.right <= frame.right)
        assertTrue(action.top >= frame.top)
        assertTrue(action.bottom <= frame.bottom)
        assertTrue(action.height >= with(compose.density) { 48.dp.toPx() })
    }

    private fun setWelcome(
        onContinue: () -> Unit = {},
        onOpenLink: (String) -> Unit = {},
    ) {
        compose.setContent {
            FriendOrFoeTheme {
                WelcomeContent(
                    WelcomeActions(
                        onGetStarted = onContinue,
                        onOpenLink = onOpenLink,
                    ),
                )
            }
        }
    }
}
