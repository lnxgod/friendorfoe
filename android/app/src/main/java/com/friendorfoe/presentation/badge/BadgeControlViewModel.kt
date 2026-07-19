package com.friendorfoe.presentation.badge

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThemeProfileStore
import com.friendorfoe.data.badge.BadgeUsbRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

@HiltViewModel
class BadgeControlViewModel @Inject constructor(
    private val repository: BadgeUsbRepository,
    private val profiles: BadgeThemeProfileStore,
) : ViewModel() {
    val badgeState = repository.state
    val themeProfiles = profiles.profiles

    fun grantUsbAccess() = repository.requestConnection()
    fun refresh() = repository.requestStatus()
    fun displayNav(action: String) = repository.displayNav(action)
    fun setMode(mode: String) = repository.setMode(mode)
    fun applyTheme(theme: BadgeTheme) = repository.applyBadgeTheme(theme)
    fun resetTheme() = repository.resetBadgeTheme()
    fun applyDisplayPolicy(policy: BadgeDisplayPolicy) = repository.applyDisplayPolicy(policy)
    fun resetDisplayPolicy() = repository.resetDisplayPolicy()
    fun createProfile(name: String, theme: BadgeTheme) = profiles.create(name, theme)
    fun renameProfile(id: String, name: String) = profiles.rename(id, name)
    fun replaceProfile(id: String, theme: BadgeTheme) = profiles.replace(id, theme)
    fun deleteProfile(id: String) = profiles.delete(id)

    fun execute(action: BadgeDangerAction) = when (action) {
        BadgeDangerAction.REBOOT -> repository.rebootBadge()
        BadgeDangerAction.BOOTLOADER -> repository.enterBootloader()
        BadgeDangerAction.RECOVER_SLOT_0 -> repository.relayScannerFirmware("ble")
        BadgeDangerAction.RECOVER_SLOT_1 -> repository.relayScannerFirmware("wifi")
    }
}
