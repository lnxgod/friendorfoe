package com.friendorfoe.presentation.about

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.repository.AppUpdateMetadata
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class AboutLandingScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun landingShowsTheAboutIdentityEvidenceCaveatAndActions() {
        var settingsOpened = 0

        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    actions = AboutLandingActions(
                        onOpenSettings = { settingsOpened++ },
                    ),
                )
            }
        }

        compose.onNodeWithTag("about_landing").assertIsDisplayed()
        compose.onNodeWithTag("about_triforce").assertIsDisplayed()
        compose.onNodeWithText("Friend or Foe").assertIsDisplayed()
        compose.onNodeWithText(
            "Were you at our DEF CON talk? Thank you for coming—we're glad you're here.",
        ).assertIsDisplayed()
        compose.onNodeWithText(
            "Observations are evidence, not proof of identity, intent, or ownership.",
        ).assertIsDisplayed()
        compose.onNodeWithTag("about_app_settings").assertIsDisplayed().performClick()
        compose.onNodeWithTag("about_reference").assertIsDisplayed()
        compose.onNodeWithTag("about_contact").assertIsDisplayed()
        compose.onNodeWithTag("about_github").assertIsDisplayed()
        compose.runOnIdle { assertEquals(1, settingsOpened) }
    }

    @Test
    fun landingShowsAndOpensAvailableGitHubUpdate() {
        var opened: String? = null
        val remote = AppUpdateMetadata(
            AppVersion(null, "0.68.0"),
            "https://github.com/lnxgod/friendorfoe/releases/tag/v0.68.0",
        )

        compose.setContent {
            FriendOrFoeTheme {
                AboutLandingScreen(
                    installedVersionName = "0.67.7-android-ar-overlay-range",
                    updateState = UpdateUiState.Available(remote),
                    actions = AboutLandingActions(onOpenUpdate = { opened = it }),
                )
            }
        }

        compose.onNodeWithText("Version 0.67.7-android-ar-overlay-range")
            .performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Version 0.68.0").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("about_open_update")
            .performScrollTo().assertIsDisplayed().performClick()
        compose.onNodeWithText("Update available").assertIsDisplayed()
        compose.runOnIdle { assertEquals(remote.releaseUrl, opened) }
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
