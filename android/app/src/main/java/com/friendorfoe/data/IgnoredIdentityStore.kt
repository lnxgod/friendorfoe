package com.friendorfoe.data

import com.friendorfoe.detection.canonicalPrivacyIdentities

internal class IgnoredIdentityStore(
    private val readStructured: () -> Set<String>?,
    private val readLegacyCsv: () -> String?,
    private val persistStructured: (Set<String>) -> Unit,
) {
    @Volatile
    private var cached: Set<String>? = null

    fun get(): Set<String> {
        cached?.let { return it }
        return synchronized(this) {
            cached ?: load().also { cached = it }
        }
    }

    @Synchronized
    fun add(identities: Iterable<String>) {
        val updated = get() + canonicalPrivacyIdentities(identities)
        persistIfChanged(updated)
    }

    @Synchronized
    fun remove(identities: Iterable<String>) {
        val updated = get() - canonicalPrivacyIdentities(identities)
        persistIfChanged(updated)
    }

    private fun load(): Set<String> {
        val structured = readStructured()
        val legacyCsv = readLegacyCsv().orEmpty()
        val legacy = if (legacyCsv.isBlank()) {
            emptySet()
        } else {
            legacyCsv.split(',').toSet()
        }
        val canonical = canonicalPrivacyIdentities(structured.orEmpty() + legacy)
        if (structured == null || legacyCsv.isNotBlank() || canonical != structured) {
            persistStructured(canonical)
        }
        return canonical
    }

    private fun persistIfChanged(updated: Set<String>) {
        val snapshot = updated.toSet()
        if (snapshot == cached) return
        persistStructured(snapshot)
        cached = snapshot
    }
}
