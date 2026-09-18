package com.friendorfoe.presentation.privacy

import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.platform.app.InstrumentationRegistry
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import java.io.File

class PrivacyEncounterScreenTest {
    @get:Rule val compose = createComposeRule()
    private val key = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "demo-tracker")
    private val finding = PrivacyFinding(
        displayId = "demo", observationKey = key, source = key.source, stableSourceId = "demo",
        routableKey = key, title = "Nearby tracker", evidence = "Repeated tracker advertisements observed by this phone.",
        limitation = "Ownership and intent are unknown.", category = PrivacyCategory.BLE_TRACKER,
        severity = FindingSeverity.AWARENESS, ownership = Ownership.UNKNOWN, signalDbm = -55,
        firstSeenWallMs = null, lastSeenWallMs = null, lastObservedElapsedMs = 100_000,
        protocolTtlMs = null, hasLiveLocalSamples = true,
    )
    private fun encounter(): PrivacyEncounter {
        val now = System.currentTimeMillis()
        return PrivacyEncounter(key, finding, now - 60_000, now, 100_000, 12,
            (0..11).map { PrivacySignalSample(it * 5_000L, now - 60_000 + it * 5_000L, -80 + it * 2) })
    }

    @Test fun expiredFindingRetainsClearlyLabeledSavedEvidenceAndSignalHistory() {
        compose.setContent {
            FriendOrFoeTheme { PrivacyFindingDetailsContent(PrivacyFindingLookupState.Expired, {}, {}, encounter()) }
        }
        compose.onNodeWithText("Item no longer current").assertIsDisplayed()
        compose.onNodeWithText("Saved observation").assertIsDisplayed()
        compose.onNodeWithText("Copy evidence").performScrollTo().performClick()
        compose.onNodeWithText("Evidence copied").assertIsDisplayed()
        compose.onNodeWithTag("privacy_signal_history").performScrollTo().assertIsDisplayed()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, "privacy-evidence.png").outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
    }

    @Test fun encounterReviewUsesExactKeyAndClearRequiresConfirmation() {
        var opened: PrivacyFindingKey? = null
        var cleared = false
        compose.setContent {
            FriendOrFoeTheme { PrivacyEncountersContent(listOf(encounter()), {}, { opened = it }, { cleared = true }) }
        }
        compose.onNodeWithText("Review evidence").performClick()
        compose.runOnIdle { assertEquals(key, opened) }
        compose.onNodeWithText("Clear recent encounters").performClick()
        compose.runOnIdle { assertFalse(cleared) }
        compose.onNodeWithText("Cancel").performClick()
        compose.runOnIdle { assertFalse(cleared) }
        compose.onNodeWithText("Clear recent encounters").performClick()
        compose.onNodeWithText("Clear", useUnmergedTree = true).performClick()
        compose.runOnIdle { assertTrue(cleared) }
    }
    @Test fun repeatedAndOwnedFiltersAndSearchCanBeResetWithoutDeletingEvidence() {
        val now = System.currentTimeMillis()
        val repeated = encounter().copy(periods = listOf(
            PrivacyObservationPeriod(1000, 2000, now - 180_000, now - 179_000, 2),
            PrivacyObservationPeriod(122_000, 123_000, now - 59_000, now - 58_000, 2)))
        val owned = encounter().copy(key = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "owned"),
            finding = finding.copy(title = "My device", ownership = Ownership.OWNED))
        compose.setContent { FriendOrFoeTheme { PrivacyEncountersContent(listOf(repeated, owned), {}, {}, {}) } }
        compose.onNodeWithText("My device").assertDoesNotExist()
        compose.onNodeWithText("Repeated").performClick()
        compose.onNodeWithText("2 observation periods with multiple updates").assertIsDisplayed()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, "privacy-repeated.png").outputStream().use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        compose.onNodeWithTag("encounter_search").performTextInput("missing")
        compose.onNodeWithText("No encounters match these filters").assertIsDisplayed()
        compose.onNodeWithText("Show all encounters").performClick()
        compose.onNodeWithText("My device").performScrollTo().assertIsDisplayed()
    }

}
