package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeTheme

internal data class BadgeControlDraftReset(
    val theme: BadgeTheme? = null,
    val displayPolicy: BadgeDisplayPolicy? = null,
)

internal fun badgeThemeEditorRefreshReset(
    appliedTheme: BadgeTheme,
): BadgeControlDraftReset = BadgeControlDraftReset(theme = appliedTheme)

internal fun badgeFilterEditorRefreshReset(
    appliedPolicy: BadgeDisplayPolicy,
): BadgeControlDraftReset = BadgeControlDraftReset(displayPolicy = appliedPolicy)

internal fun badgeStatusRefreshReset(
    appliedTheme: BadgeTheme,
    appliedPolicy: BadgeDisplayPolicy,
): BadgeControlDraftReset = BadgeControlDraftReset(
    theme = appliedTheme,
    displayPolicy = appliedPolicy,
)
