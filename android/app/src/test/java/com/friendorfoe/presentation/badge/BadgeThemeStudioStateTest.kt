package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeThemeColorCodec
import com.friendorfoe.data.badge.BadgeThemeProfile
import com.friendorfoe.data.badge.Rgb888
import com.friendorfoe.data.badge.badgeThemePresetById
import com.friendorfoe.data.badge.defaultBadgeTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class BadgeThemeStudioStateTest {

    @Test
    fun `expanded studio bounds scrolling content and sticky actions together`() {
        assertEquals(480, BadgeThemeStudioExpandedMaxHeightDp)
        assertEquals(
            listOf(
                BadgeThemeStudioExpandedRegion.ScrollableContent,
                BadgeThemeStudioExpandedRegion.StickyActions,
            ),
            BadgeThemeStudioExpandedRegions,
        )
    }

    @Test
    fun `pending profile id resolves to current profile instead of stale snapshot`() {
        val stale = BadgeThemeProfile(
            id = "ops",
            name = "Old name",
            theme = defaultBadgeTheme(),
        )
        val current = stale.copy(
            name = "Current name",
            theme = badgeThemePresetById("blacklight")!!.theme,
        )

        assertEquals(
            BadgeThemeProfileResolution.Found(current),
            resolveBadgeThemeProfile(listOf(current), stale.id),
        )
        assertEquals(
            BadgeThemeProfileResolution.Missing,
            resolveBadgeThemeProfile(listOf(current), "deleted"),
        )
        assertEquals("Profile no longer exists.", BadgeThemeProfileMissingMessage)
    }

    @Test
    fun `profile mutation race reports a generic failure instead of a false no-op`() {
        BadgeThemeProfileMutation.entries.forEach { mutation ->
            val message = badgeThemeProfileMutationFailureMessage(mutation)

            assertEquals(
                "Profile could not be ${mutation.failureVerb}. " +
                    "It may have changed or no longer exists.",
                message,
            )
            assertFalse(message.contains("matches", ignoreCase = true))
        }
    }

    @Test
    fun `segmented options expose selected semantics and state description`() {
        assertEquals(
            BadgeThemeSelectedOptionSemantics(
                selected = true,
                stateDescription = "Selected",
            ),
            badgeThemeSelectedOptionSemantics(value = "night", selectedValue = "night"),
        )
        assertEquals(
            BadgeThemeSelectedOptionSemantics(
                selected = false,
                stateDescription = "Not selected",
            ),
            badgeThemeSelectedOptionSemantics(value = "field", selectedValue = "night"),
        )
    }

    @Test
    fun `preview keeps exactly two global one BLE and one WiFi lane`() {
        assertEquals(
            listOf(
                BadgeThemePreviewLaneKind.Global,
                BadgeThemePreviewLaneKind.Global,
                BadgeThemePreviewLaneKind.Ble,
                BadgeThemePreviewLaneKind.Wifi,
            ),
            BadgeThemePreviewLanes.map { it.kind },
        )
    }

    @Test
    fun `preset selection replaces the complete local draft without a command`() {
        val selected = badgeThemePresetById("blacklight")!!.theme

        val transition = reduceBadgeThemeStudio(
            draft = defaultBadgeTheme(),
            action = BadgeThemeStudioAction.SelectDraft(selected),
        )

        assertEquals(selected, transition.draft)
        assertEquals(BadgeThemeStudioCommand.None, transition.command)
    }

    @Test
    fun `accent edit quantizes RGB888 into the local wire draft`() {
        val transition = reduceBadgeThemeStudio(
            draft = defaultBadgeTheme(),
            action = BadgeThemeStudioAction.SetAccent(
                key = "meta",
                color = Rgb888(255, 76, 169),
            ),
        )

        val effective = transition.draft.accents.getValue("meta")
        assertEquals(0xFA75, effective)
        assertEquals("#FF4CAC", BadgeThemeColorCodec.effectiveHex(effective))
        assertEquals(BadgeThemeStudioCommand.None, transition.command)
    }

    @Test
    fun `reset changes only the local draft`() {
        val selected = badgeThemePresetById("inferno")!!.theme

        val transition = reduceBadgeThemeStudio(
            draft = selected,
            action = BadgeThemeStudioAction.ResetDraft,
        )

        assertEquals(defaultBadgeTheme(), transition.draft)
        assertEquals(BadgeThemeStudioCommand.None, transition.command)
    }

    @Test
    fun `only apply transition requests badge transport`() {
        val draft = badgeThemePresetById("obsidian_gold")!!.theme
        val localActions = listOf(
            BadgeThemeStudioAction.SelectDraft(badgeThemePresetById("field")!!.theme),
            BadgeThemeStudioAction.SetAccent("clear", Rgb888(1, 2, 3)),
            BadgeThemeStudioAction.ResetDraft,
        )

        localActions.forEach { action ->
            assertEquals(
                BadgeThemeStudioCommand.None,
                reduceBadgeThemeStudio(draft, action).command,
            )
        }
        assertEquals(
            BadgeThemeStudioCommand.Refresh,
            reduceBadgeThemeStudio(draft, BadgeThemeStudioAction.Refresh).command,
        )
        assertEquals(
            BadgeThemeStudioCommand.Apply,
            reduceBadgeThemeStudio(draft, BadgeThemeStudioAction.Apply).command,
        )
    }
}
