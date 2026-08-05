package com.friendorfoe.presentation.badge

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Rule
import org.junit.Test

class BadgeHeaderTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun headerShowsBadgeTitleAndTriforce() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeHeader()
            }
        }

        compose.onNodeWithText("Badge").assertIsDisplayed()
        compose.onNodeWithTag("badge_triforce").assertIsDisplayed()
    }
}
