package com.friendorfoe.data.badge

import android.content.Context
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.Locale
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

data class BadgeThemeProfile(
    val id: String,
    val name: String,
    val theme: BadgeTheme,
)

internal class BadgeThemeProfileLibrary(
    readEncoded: () -> String?,
    private val persistEncoded: (String) -> Unit,
    private val idFactory: () -> String = { UUID.randomUUID().toString() },
) {
    var profiles: List<BadgeThemeProfile> = BadgeThemeProfileCodec.decode(readEncoded())
        private set

    fun create(name: String, theme: BadgeTheme): Boolean {
        val normalizedName = normalizeProfileName(name) ?: return false
        if (profiles.any { it.name.equals(normalizedName, ignoreCase = true) }) return false
        val id = idFactory()
        if (id.isBlank() || profiles.any { it.id == id }) return false
        return persist(
            profiles + BadgeThemeProfile(
                id = id,
                name = normalizedName,
                theme = theme.normalizedV1(),
            ),
        )
    }

    fun rename(id: String, name: String): Boolean {
        val index = profiles.indexOfFirst { it.id == id }
        if (index < 0) return false
        val normalizedName = normalizeProfileName(name) ?: return false
        if (profiles[index].name == normalizedName) return false
        if (profiles.anyIndexed { otherIndex, profile ->
                otherIndex != index && profile.name.equals(normalizedName, ignoreCase = true)
            }
        ) return false
        return persist(
            profiles.toMutableList().apply {
                this[index] = this[index].copy(name = normalizedName)
            },
        )
    }

    fun replace(id: String, theme: BadgeTheme): Boolean {
        val index = profiles.indexOfFirst { it.id == id }
        if (index < 0) return false
        val normalizedTheme = theme.normalizedV1()
        if (profiles[index].theme == normalizedTheme) return false
        return persist(
            profiles.toMutableList().apply {
                this[index] = this[index].copy(theme = normalizedTheme)
            },
        )
    }

    fun delete(id: String): Boolean {
        val index = profiles.indexOfFirst { it.id == id }
        if (index < 0) return false
        return persist(profiles.filterIndexed { itemIndex, _ -> itemIndex != index })
    }

    private fun persist(updated: List<BadgeThemeProfile>): Boolean {
        persistEncoded(BadgeThemeProfileCodec.encode(updated))
        profiles = updated
        return true
    }
}

@Singleton
class BadgeThemeProfileStore @Inject constructor(
    @ApplicationContext context: Context,
) {
    private val preferences = context.getSharedPreferences(PREFERENCES_FILE, Context.MODE_PRIVATE)
    private val library = BadgeThemeProfileLibrary(
        readEncoded = { preferences.getString(PROFILES_KEY, null) },
        persistEncoded = { encoded ->
            preferences.edit().putString(PROFILES_KEY, encoded).apply()
        },
    )
    private val mutableProfiles = MutableStateFlow(library.profiles)

    val profiles: StateFlow<List<BadgeThemeProfile>> = mutableProfiles.asStateFlow()

    fun create(name: String, theme: BadgeTheme): Boolean =
        updateProfiles { library.create(name, theme) }

    fun rename(id: String, name: String): Boolean =
        updateProfiles { library.rename(id, name) }

    fun replace(id: String, theme: BadgeTheme): Boolean =
        updateProfiles { library.replace(id, theme) }

    fun delete(id: String): Boolean =
        updateProfiles { library.delete(id) }

    private inline fun updateProfiles(operation: () -> Boolean): Boolean {
        val changed = operation()
        if (changed) mutableProfiles.value = library.profiles
        return changed
    }

    private companion object {
        const val PREFERENCES_FILE = "fof_badge_theme_profiles"
        const val PROFILES_KEY = "profiles_v1"
    }
}

private object BadgeThemeProfileCodec {
    private const val VERSION = 1

    fun decode(encoded: String?): List<BadgeThemeProfile> {
        if (encoded.isNullOrBlank()) return emptyList()
        val root = runCatching { JsonParser.parseString(encoded).asJsonObject }.getOrNull()
            ?: return emptyList()
        val version = runCatching { root.requiredInt("version") }.getOrNull()
            ?: return emptyList()
        if (version != VERSION) return emptyList()
        val encodedProfiles = root.get("profiles")
            ?.takeIf { it.isJsonArray }
            ?.asJsonArray
            ?: return emptyList()

        val ids = mutableSetOf<String>()
        val names = mutableSetOf<String>()
        return buildList {
            encodedProfiles.forEach { element ->
                val profile = decodeProfile(
                    element.takeIf { it.isJsonObject }?.asJsonObject ?: return@forEach,
                ) ?: return@forEach
                val foldedName = profile.name.lowercase(Locale.ROOT)
                if (profile.id in ids || foldedName in names) return@forEach
                ids += profile.id
                names += foldedName
                add(profile)
            }
        }
    }

    fun encode(profiles: List<BadgeThemeProfile>): String = JsonObject().apply {
        addProperty("version", VERSION)
        add("profiles", JsonArray().apply {
            profiles.forEach { profile ->
                add(JsonObject().apply {
                    addProperty("id", profile.id)
                    addProperty("name", profile.name)
                    add("theme", profile.theme.normalizedV1().toJsonObject())
                })
            }
        })
    }.toString()

    private fun decodeProfile(encoded: JsonObject): BadgeThemeProfile? = runCatching {
        val id = encoded.requiredString("id")
        require(id.isNotBlank())
        val name = normalizeProfileName(encoded.requiredString("name"))
            ?: error("invalid profile name")
        val theme = encoded.get("theme")
            ?.takeIf { it.isJsonObject }
            ?.asJsonObject
            ?: error("invalid profile theme")
        BadgeThemeProfile(
            id = id,
            name = name,
            theme = decodeTheme(theme),
        )
    }.getOrNull()

    private fun decodeTheme(encoded: JsonObject): BadgeTheme {
        val defaults = defaultBadgeTheme()
        val palette = encoded.optionalString("palette", defaults.palette)
        require(palette in BadgeThemePalettes)
        val background = encoded.optionalString("background", defaults.background)
        val brightness = encoded.optionalInt("brightness", defaults.brightness)
        val version = encoded.optionalInt("version", defaults.version)
        val encodedAccents = encoded.get("accents")?.let { accents ->
            require(accents.isJsonObject)
            accents.asJsonObject
        }
        val accents = BadgeThemeAccentClasses.associate { accent ->
            accent.key to (encodedAccents?.optionalInt(accent.key, accent.defaultRgb565)
                ?: accent.defaultRgb565)
        }
        return BadgeTheme(
            version = version,
            palette = palette,
            background = background,
            brightness = brightness,
            accents = accents,
        ).normalizedV1()
    }

    private fun JsonObject.requiredString(key: String): String {
        val primitive = get(key)
            ?.takeIf { it.isJsonPrimitive }
            ?.asJsonPrimitive
            ?: error("missing $key")
        require(primitive.isString)
        return primitive.asString
    }

    private fun JsonObject.requiredInt(key: String): Int {
        val primitive = get(key)
            ?.takeIf { it.isJsonPrimitive }
            ?.asJsonPrimitive
            ?: error("missing $key")
        require(primitive.isNumber)
        return primitive.asString.toIntOrNull() ?: error("invalid $key")
    }

    private fun JsonObject.optionalString(key: String, default: String): String =
        if (has(key)) requiredString(key) else default

    private fun JsonObject.optionalInt(key: String, default: Int): Int =
        if (has(key)) requiredInt(key) else default
}

private fun normalizeProfileName(name: String): String? =
    name.trim().takeIf { it.length in 1..32 }

private inline fun <T> Iterable<T>.anyIndexed(predicate: (Int, T) -> Boolean): Boolean {
    forEachIndexed { index, item ->
        if (predicate(index, item)) return true
    }
    return false
}
