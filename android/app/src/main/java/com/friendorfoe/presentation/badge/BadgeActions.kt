package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeTheme

data class BadgeActions(
    val refresh: () -> Unit,
    val reconnect: () -> Unit,
    val updateTheme: ((BadgeTheme) -> BadgeTheme) -> Unit,
    val updatePolicy: ((BadgeDisplayPolicy) -> BadgeDisplayPolicy) -> Unit,
    val updateNetworkMode: (BadgeNetworkMode) -> Unit,
    val useDefaults: () -> Unit,
    val revert: () -> Unit,
    val apply: () -> Unit,
    val navigateDisplay: (BadgeDisplayAction) -> Unit,
    val requestRecovery: (BadgeRecoveryAction) -> Unit,
    val openDiagnostics: () -> Unit,
    val openRecovery: () -> Unit,
)
