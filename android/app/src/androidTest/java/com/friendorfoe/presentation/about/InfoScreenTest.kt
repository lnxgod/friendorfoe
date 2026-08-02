package com.friendorfoe.presentation.about

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performScrollToIndex
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.repository.SessionHealth
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import com.friendorfoe.presentation.permissions.PermissionUiState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class InfoScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun sectionsAndAdvancedEntriesUseTheApprovedOrder() {
        assertEquals(
            listOf(
                "Source & permission status",
                "Settings",
                "Guide & category legend",
                "Privacy & Data",
                "About, support, version & updates",
                "Advanced",
            ),
            INFO_SECTION_TITLES,
        )
        setInfoContent()

        compose.onNodeWithTag("info_section_0").assertIsDisplayed()
        scrollToSection(5)
        compose.onNodeWithTag("info_section_5").assertIsDisplayed()
        compose.onNodeWithTag("advanced_magnetic_field").performScrollTo().assertIsEnabled()
        compose.onNodeWithTag("advanced_ir_like_light").performScrollTo().assertIsEnabled()
        compose.onNodeWithTag("calibration_entry").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Unavailable").assertIsDisplayed()
    }

    @Test
    fun wholeSettingsRowUsesLiveToggleState() {
        var enabled by mutableStateOf(false)
        compose.setContent {
            FriendOrFoeTheme {
                InfoContent(
                    state = state(phonePrivacyEnabled = enabled),
                    actions = InfoActions(
                        onSetSetting = { key, value ->
                            if (key == InfoSettingKey.PHONE_PRIVACY_SCAN) enabled = value
                        },
                    ),
                )
            }
        }

        compose.onNodeWithTag("setting_phone_privacy_scan")
            .performScrollTo()
            .performClick()
            .assertIsOn()
    }

    @Test
    fun freshStateShowsBackendOffAndPreventsBackendOnlyMode() {
        val settings = DetectionSettings.defaults()
        setInfoContent(
            state = state().copy(
                settings = settings,
                backendUrlCanTest = false,
            ),
            actions = InfoActions(
                settingDisabledReason = { key ->
                    infoSettingDisabledReason(
                        key = key,
                        settings = settings,
                        phonePrivacyPermission = PermissionUiState.Granted,
                    )
                },
            ),
        )

        scrollToSection(1)
        compose.onNodeWithTag("setting_sensor_backend")
            .performScrollTo()
            .assertIsOff()
        compose.onNodeWithTag("setting_backend_only")
            .performScrollTo()
            .assertIsNotEnabled()
        compose.onNodeWithText(
            "Enable Sensor backend connection first.",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithTag("backend_test").assertIsNotEnabled()
    }

    @Test
    fun invalidBackendCannotSaveAndShowsInlineRecovery() {
        var saved = 0
        setInfoContent(
            state = state().copy(
                backendUrlDraft = "not-a-url",
                backendUrlError = "Enter a complete http:// or https:// URL",
                backendUrlCanSave = false,
                backendUrlCanTest = false,
            ),
            actions = InfoActions(onSaveBackendUrl = { saved++ }),
        )

        compose.onNodeWithTag("backend_save").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Enter a complete http:// or https:// URL")
            .assertIsDisplayed()
        compose.runOnIdle { assertEquals(0, saved) }
    }

    @Test
    fun privacyDataCopyMatchesActualAndroidBehavior() {
        setInfoContent()

        scrollToSection(3)
        listOf(
            "History may store observations and phone coordinates locally.",
            "ADS-B and weather services may receive location when those features are used.",
            "A configured sensor backend exchanges detection data with this app.",
            "Calibration sends operator/session GPS to the configured backend when used.",
        ).forEach { fact ->
            compose.onNodeWithText(fact).assertIsDisplayed()
        }
        listOf(
            "No personal data is collected or transmitted",
            "All detection data stays on your device",
        ).forEach { falseClaim ->
            assertEquals(
                0,
                compose.onAllNodesWithText(falseClaim, substring = true, ignoreCase = true)
                    .fetchSemanticsNodes().size,
            )
        }
    }

    @Test
    fun connectionCopyNamesTheExactConfiguredEndpoint() {
        val endpoint = BackendEndpoint.parse("https://field-kit.example:8443/").getOrThrow()
        setInfoContent(
            state = state().copy(
                settings = state().settings.copy(backendUrl = endpoint.baseUrl),
                backendUrlDraft = endpoint.baseUrl,
                connection = ConnectionTestState.Connected(endpoint, "0.65.0"),
                sessionHealth = SessionHealth.Healthy(endpoint),
                calibrationEntryAvailable = true,
            ),
        )

        scrollToSection(1)
        compose.onNodeWithTag("backend_connection_status")
            .performScrollTo()
            .assertTextContains(endpoint.baseUrl, substring = true)
            .assertIsDisplayed()
    }

    @Test
    fun calibrationOnlyNavigatesAfterExactSessionHealth() {
        val endpoint = BackendEndpoint.parse("http://badge-lab:8000/").getOrThrow()
        var opened = 0
        setInfoContent(
            state = state().copy(
                settings = state().settings.copy(
                    sensorBackendEnabled = true,
                    backendUrl = endpoint.baseUrl,
                ),
                sessionHealth = SessionHealth.Healthy(endpoint),
                calibrationEntryAvailable = true,
            ),
            actions = InfoActions(onOpenCalibration = { opened++ }),
        )

        scrollToSection(5)
        compose.onNodeWithTag("calibration_entry")
            .assertIsEnabled()
            .performClick()
        compose.runOnIdle { assertEquals(1, opened) }
    }

    @Test
    fun updateFailureIsNeutralAndNeverFabricatesAvailability() {
        setInfoContent(
            state = state().copy(
                updateState = UpdateUiState.Failed("Could not check for updates"),
            ),
        )

        scrollToSection(4)
        compose.onNodeWithText("Could not check for updates")
            .assertIsDisplayed()
        assertEquals(
            0,
            compose.onAllNodesWithText("Update available", substring = true)
                .fetchSemanticsNodes().size,
        )
    }

    @Test
    fun advancedSafetyCopyWrapsAndActionsStayInCompactBoundsAtOnePointThreeText() {
        assertAdvancedRowsFit(fontScale = 1.3f)
    }

    @Test
    fun advancedSafetyCopyWrapsAndActionsStayInCompactBoundsAtTwoTimesText() {
        assertAdvancedRowsFit(fontScale = 2f)
    }

    private fun assertAdvancedRowsFit(fontScale: Float) {
        compose.setContent {
            val currentDensity = LocalDensity.current
            CompositionLocalProvider(
                LocalDensity provides Density(currentDensity.density, fontScale),
            ) {
                FriendOrFoeTheme {
                    Box(Modifier.width(360.dp).height(700.dp)) {
                        InfoContent(
                            state = state().copy(calibrationEntryAvailable = true),
                            actions = InfoActions(),
                        )
                    }
                }
            }
        }

        scrollToSection(5)
        val frame = compose.onNodeWithTag("info_list").fetchSemanticsNode().boundsInRoot
        listOf(
            "advanced_magnetic_field",
            "advanced_ir_like_light",
            "calibration_entry",
        ).forEach { tag ->
            val bounds = compose.onNodeWithTag(tag)
                .performScrollTo()
                .assertIsDisplayed()
                .fetchSemanticsNode().boundsInRoot
            assertTrue(bounds.left >= frame.left)
            assertTrue(bounds.right <= frame.right)
            assertTrue(bounds.height >= with(compose.density) { 48.dp.toPx() })
        }
        compose.onNodeWithTag(
            "advanced_magnetic_field_description",
            useUnmergedTree = true,
        )
            .performScrollTo()
            .assertIsDisplayed()
        compose.onNodeWithTag(
            "advanced_ir_like_light_description",
            useUnmergedTree = true,
        )
            .performScrollTo()
            .assertIsDisplayed()
        compose.onNodeWithTag(
            "calibration_entry_description",
            useUnmergedTree = true,
        )
            .performScrollTo()
            .assertIsDisplayed()
    }

    private fun setInfoContent(
        state: InfoUiState = state(),
        actions: InfoActions = InfoActions(),
    ) {
        compose.setContent {
            FriendOrFoeTheme { InfoContent(state, actions) }
        }
    }

    private fun scrollToSection(index: Int) {
        compose.onNodeWithTag("info_list").performScrollToIndex(index + 1)
    }

    private fun state(phonePrivacyEnabled: Boolean = false): InfoUiState = InfoUiState(
        settings = DetectionSettings.defaults().copy(
            phonePrivacyScanEnabled = phonePrivacyEnabled,
        ),
        backendUrlDraft = DetectionSettings.defaults().backendUrl,
        backendUrlCanSave = true,
        backendUrlCanTest = true,
        installedVersion = AppVersion(108, "0.64.65"),
    )
}
