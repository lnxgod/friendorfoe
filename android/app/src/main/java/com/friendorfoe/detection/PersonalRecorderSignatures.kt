package com.friendorfoe.detection

/** Passive hints only: these signals do not establish that recording is active.
 * See docs/personal-ai-recorders.md for provenance and ambiguous exclusions.
 */
internal object PersonalRecorderSignatures {
    data class Match(val manufacturer: String, val confidence: Float, val reason: String)
    private data class Signature(val brand: String, val names: List<String>, val uuid: String?)
    private val signatures = listOf(
        Signature("Plaud", listOf("plaud", "notepin"), null),
        Signature("Omi", listOf("omi"), "19b10000-e8f2-537e-4f6c-d104768a1214"),
        Signature("Limitless", listOf("limitless"), "632de001-604c-446b-a80f-7963e950f3fb"),
        Signature("Bee", listOf("bee"), "03d5d5c4-a86c-11ee-9d89-8f2089a49e7e"),
        Signature("Friend", listOf("friend_"), "1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da"),
        Signature("Fieldy", listOf("fieldy"), null),
    )

    fun match(name: String?, services: List<String> = emptyList()): Match? {
        for (signature in signatures) {
            if (signature.uuid != null && services.any { it.equals(signature.uuid, ignoreCase = true) }) {
                return Match(signature.brand, 0.90f, "recorder:uuid:${signature.uuid}")
            }
        }
        val localName = name.orEmpty().trim()
        if (Regex("plaud[a-z0-9]{4}", RegexOption.IGNORE_CASE).matches(localName)) {
            return Match("Plaud", 0.75f, "recorder:name:plaud_serial")
        }
        for (signature in signatures) {
            for (prefix in signature.names) {
                if (localName.equals(prefix, ignoreCase = true) ||
                    (localName.startsWith(prefix, ignoreCase = true) &&
                        (prefix.endsWith("_") || localName.getOrNull(prefix.length) in listOf(' ', '-', '_')))) {
                    return Match(signature.brand, 0.75f, "recorder:name:$prefix")
                }
            }
        }
        return null
    }
}
