package com.friendorfoe.presentation.ar

import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class ArServicesNoticeTest {
    @get:Rule val compose = createComposeRule()

    @Test fun disclosurePrecedesArAndLinksDoNotStartCamera() {
        val acknowledged = mutableStateOf(false)
        val targets = mutableListOf<String>()
        var nearby = 0
        compose.setContent {
            FriendOrFoeTheme {
                ArServicesEntryContent(acknowledged.value, { acknowledged.value = true }, { nearby++ },
                    { targets.add(it) }) { Text("AR route") }
            }
        }
        compose.onNodeWithText(ARCORE_NOTICE).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Google Play Services for AR").performScrollTo().performClick()
        compose.onNodeWithText("Google Privacy Policy").performScrollTo().performClick()
        compose.onNodeWithText("Use Nearby instead").performScrollTo().performClick()
        compose.runOnIdle {
            assertEquals(listOf("https://play.google.com/store/apps/details?id=com.google.ar.core",
                "https://policies.google.com/privacy"), targets)
            assertEquals(1, nearby)
        }
        compose.onNodeWithText("AR route").assertDoesNotExist()
        compose.onNodeWithText("Continue to AR").performScrollTo().performClick()
        compose.onNodeWithText("AR route").assertIsDisplayed()
        compose.onNodeWithText(ARCORE_NOTICE).assertDoesNotExist()
    }
}
