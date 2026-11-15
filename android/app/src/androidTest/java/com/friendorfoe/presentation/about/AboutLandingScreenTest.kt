package com.friendorfoe.presentation.about

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import com.friendorfoe.data.AppVersion
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class AboutLandingScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun moreShowsDailyToolsAndKeepsSettingsAction() {
        var settingsOpened = 0
        var sourceUrl: String? = null

        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    actions = AboutLandingActions(
                        onOpenSettings = { settingsOpened++ },
                        onOpenSource = { sourceUrl = it },
                    ),
                )
            }
        }

        compose.onNodeWithTag("about_landing").assertIsDisplayed()
        compose.onNodeWithText("Friend or Foe").assertIsDisplayed()
        compose.onNodeWithTag("about_app_settings").assertIsDisplayed().performClick()
        compose.onNodeWithTag("more_history").assertIsDisplayed()
        compose.onNodeWithTag("more_badge").assertIsDisplayed()
        compose.onNodeWithTag("about_reference").assertIsDisplayed()
        compose.onNodeWithTag("about_contact").assertDoesNotExist()
        compose.onNodeWithText("Observations are evidence, not proof of identity, intent, or ownership.")
            .performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("about_github").performScrollTo().assertIsDisplayed().performClick()
        compose.runOnIdle { assertEquals("https://github.com/lnxgod/friendorfoe", sourceUrl) }
        compose.runOnIdle { assertEquals(1, settingsOpened) }
    }

    @Test
    fun availableUpdateOpensThePlayListing() {
        var opens = 0
        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    updateState = UpdateUiState.Available,
                    actions = AboutLandingActions(onOpenUpdate = { opens++ }),
                )
            }
        }
        compose.onNodeWithTag("about_open_update").performScrollTo().performClick()
        compose.runOnIdle { assertEquals(1, opens) }
    }

    @Test
    fun landingShowsCheckingWithoutASecondAction() {
        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    actions = AboutLandingActions(),
                    updateState = UpdateUiState.Checking,
                )
            }
        }

        compose.onNodeWithTag("about_check_updates").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Checking for updates").assertIsDisplayed()
    }

    @Test
    fun landingShowsUpToDateAndAllowsCheckAgain() {
        var checks = 0
        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    actions = AboutLandingActions(onCheckForUpdates = { checks++ }),
                    updateState = UpdateUiState.UpToDate(AppVersion(120, "0.67.7")),
                )
            }
        }

        compose.onNodeWithTag("about_check_updates").performScrollTo().performClick()
        compose.onNodeWithText("Up to date").assertIsDisplayed()
        compose.runOnIdle { assertEquals(1, checks) }
    }

    @Test
    fun landingFailureShowsRetryAndInvokesCallback() {
        var checks = 0
        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    actions = AboutLandingActions(onCheckForUpdates = { checks++ }),
                    updateState = UpdateUiState.Failed("Could not check for updates"),
                )
            }
        }

        compose.onNodeWithTag("about_check_updates").performScrollTo().performClick()
        compose.onNodeWithText("Could not check for updates").assertIsDisplayed()
        compose.runOnIdle { assertEquals(1, checks) }
    }
}
