package com.friendorfoe.presentation.about

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
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
}
