package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.badgeThemePresetById
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicy
import com.friendorfoe.data.badge.withClassEnabled
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BadgeControlDraftRefreshTest {

    private val appliedTheme = badgeThemePresetById("blacklight")!!.theme
    private val appliedPolicy = defaultBadgeDisplayPolicy()
        .withClassEnabled("tracker", enabled = false)

    @Test
    fun `theme editor refresh restores only the cached applied theme`() {
        val reset = badgeThemeEditorRefreshReset(appliedTheme)

        assertEquals(appliedTheme, reset.theme)
        assertNull(reset.displayPolicy)
    }

    @Test
    fun `filter editor refresh restores only the cached applied display policy`() {
        val reset = badgeFilterEditorRefreshReset(appliedPolicy)

        assertNull(reset.theme)
        assertEquals(appliedPolicy, reset.displayPolicy)
    }

    @Test
    fun `top level status refresh restores both cached applied drafts`() {
        assertEquals(
            BadgeControlDraftReset(
                theme = appliedTheme,
                displayPolicy = appliedPolicy,
            ),
            badgeStatusRefreshReset(appliedTheme, appliedPolicy),
        )
    }
}
