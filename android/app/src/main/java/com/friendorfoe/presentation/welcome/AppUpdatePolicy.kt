package com.friendorfoe.presentation.welcome

object AppUpdatePolicy {
    private val versionPattern = Regex("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun isRemoteNewer(currentVersion: String, remoteTag: String): Boolean {
        val current = parseSemanticCore(currentVersion) ?: return false
        val remote = parseSemanticCore(remoteTag) ?: return false
        for (index in remote.indices) {
            if (remote[index] != current[index]) {
                return remote[index] > current[index]
            }
        }
        return false
    }

    private fun parseSemanticCore(version: String): List<Int>? {
        val match = versionPattern.matchEntire(version) ?: return null
        return match.groupValues.drop(1).map { it.toIntOrNull() ?: return null }
    }
}
