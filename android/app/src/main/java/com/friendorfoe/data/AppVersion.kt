package com.friendorfoe.data

data class AppVersion(
    val code: Long?,
    val name: String,
)

private val numericVersionPattern = Regex(
    "^v?(\\d+(?:\\.\\d+){1,3})(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$",
)

private fun numericVersionParts(value: String): List<Long>? {
    val match = numericVersionPattern.matchEntire(value.trim()) ?: return null
    return match.groupValues[1].split('.').map { component ->
        component.toLongOrNull()?.takeIf { it <= 999_999L } ?: return null
    }
}

internal fun isWellFormedAppVersionName(value: String): Boolean =
    numericVersionParts(value) != null

fun isUpdateAvailable(installed: AppVersion, remote: AppVersion): Boolean {
    if (installed.code != null && remote.code != null) {
        return remote.code > installed.code
    }

    val current = numericVersionParts(installed.name) ?: return false
    val latest = numericVersionParts(remote.name) ?: return false
    val width = maxOf(current.size, latest.size)
    repeat(width) { index ->
        val comparison = latest.getOrElse(index) { 0L }
            .compareTo(current.getOrElse(index) { 0L })
        if (comparison != 0) return comparison > 0
    }
    return false
}
