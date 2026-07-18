package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeThemeProfileStoreTest {

    @Test
    fun `profiles survive reload in stable order`() {
        var raw: String? = null
        val ids = sequenceOf("one", "two").iterator()
        val first = BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = { raw = it },
            idFactory = ids::next,
        )

        assertTrue(first.create("Purple Ops", preset("blacklight")))
        assertTrue(first.create("Gold Ops", preset("obsidian_gold")))

        val reloaded = BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = { raw = it },
        )
        assertEquals(listOf("one", "two"), reloaded.profiles.map { it.id })
        assertEquals(listOf("Purple Ops", "Gold Ops"), reloaded.profiles.map { it.name })
        assertEquals(
            listOf("blacklight", "obsidian_gold"),
            reloaded.profiles.map { recognizeBadgeThemePreset(it.theme)!!.id },
        )
    }

    @Test
    fun `create trims names and enforces blank and length boundaries`() {
        val library = inMemoryLibrary("one", "two")

        assertFalse(library.create("   ", defaultBadgeTheme()))
        assertTrue(library.create("  ${"a".repeat(32)}  ", defaultBadgeTheme()))
        assertEquals("a".repeat(32), library.profiles.single().name)
        assertFalse(library.create("b".repeat(33), defaultBadgeTheme()))
    }

    @Test
    fun `names are unique case insensitively across create and rename`() {
        val library = inMemoryLibrary("one", "two", "three")

        assertTrue(library.create("Field Team", defaultBadgeTheme()))
        assertFalse(library.create(" field team ", preset("blacklight")))
        assertTrue(library.create("Night Team", preset("inferno")))
        assertFalse(library.rename("two", "FIELD TEAM"))

        assertEquals(listOf("Field Team", "Night Team"), library.profiles.map { it.name })
    }

    @Test
    fun `rename trims the new name and preserves id order and theme`() {
        val library = inMemoryLibrary("one", "two")
        assertTrue(library.create("First", preset("field")))
        assertTrue(library.create("Second", preset("blacklight")))
        val originalTheme = library.profiles.first().theme

        assertTrue(library.rename("one", "  Primary  "))

        assertEquals(listOf("one", "two"), library.profiles.map { it.id })
        assertEquals(listOf("Primary", "Second"), library.profiles.map { it.name })
        assertEquals(originalTheme, library.profiles.first().theme)
    }

    @Test
    fun `replace normalizes theme and preserves id order and name`() {
        val library = inMemoryLibrary("one", "two")
        assertTrue(library.create("First", preset("field")))
        assertTrue(library.create("Second", preset("blacklight")))

        val incomplete = BadgeTheme(
            version = 9,
            palette = "night",
            background = "dim",
            brightness = 101,
            accents = mapOf("drone" to -1, "ignored" to 123),
        )
        assertTrue(library.replace("one", incomplete))

        val replaced = library.profiles.first()
        assertEquals("one", replaced.id)
        assertEquals("First", replaced.name)
        assertEquals(listOf("one", "two"), library.profiles.map { it.id })
        assertEquals(incomplete.normalizedV1(), replaced.theme)
        assertEquals(BadgeThemeAccentClasses.map { it.key }, replaced.theme.accents.keys.toList())
    }

    @Test
    fun `delete removes only the requested id and unknown ids do nothing`() {
        val persisted = mutableListOf<String>()
        val library = inMemoryLibrary("one", "two", "three", persisted = persisted)
        assertTrue(library.create("First", preset("field")))
        assertTrue(library.create("Second", preset("blacklight")))
        assertTrue(library.create("Third", preset("inferno")))
        val writesBeforeUnknowns = persisted.size

        assertFalse(library.rename("missing", "Unknown"))
        assertFalse(library.replace("missing", preset("ghostline")))
        assertFalse(library.delete("missing"))
        assertEquals(writesBeforeUnknowns, persisted.size)

        assertTrue(library.delete("two"))
        assertEquals(listOf("one", "three"), library.profiles.map { it.id })
        assertEquals(listOf("First", "Third"), library.profiles.map { it.name })
    }

    @Test
    fun `only successful state changes persist exactly once`() {
        val persisted = mutableListOf<String>()
        val library = inMemoryLibrary("one", "two", persisted = persisted)

        assertFalse(library.create("", defaultBadgeTheme()))
        assertEquals(0, persisted.size)
        assertTrue(library.create("Ops", defaultBadgeTheme()))
        assertEquals(1, persisted.size)
        assertFalse(library.create("ops", defaultBadgeTheme()))
        assertFalse(library.rename("one", "  Ops  "))
        assertFalse(library.replace("one", defaultBadgeTheme().copy(version = 99)))
        assertFalse(library.delete("missing"))
        assertEquals(1, persisted.size)

        assertTrue(library.rename("one", "OPS"))
        assertEquals(2, persisted.size)
        assertTrue(library.replace("one", preset("blacklight")))
        assertEquals(3, persisted.size)
        assertTrue(library.delete("one"))
        assertEquals(4, persisted.size)
    }

    @Test
    fun `loading never writes a repaired snapshot`() {
        val persisted = mutableListOf<String>()
        val raw = profileDocument(
            validProfile("one", "First"),
            "42",
            validProfile("two", "Second"),
        )

        val library = BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = { persisted += it },
        )

        assertEquals(listOf("one", "two"), library.profiles.map { it.id })
        assertTrue(persisted.isEmpty())
    }

    @Test
    fun `malformed entries do not discard valid siblings`() {
        val raw = profileDocument(
            validProfile("one", "First"),
            "null",
            "42",
            "{}",
            """{"id":"bad","name":9,"theme":{}}""",
            """{"id":"bad-theme","name":"Bad Theme","theme":"field"}""",
            validProfile("two", "Second", palette = "night"),
        )

        val library = readOnlyLibrary(raw)

        assertEquals(listOf("one", "two"), library.profiles.map { it.id })
        assertEquals(listOf("First", "Second"), library.profiles.map { it.name })
    }

    @Test
    fun `malformed documents and unsupported top level versions load empty`() {
        listOf<String?>(
            null,
            "",
            "not json",
            "[]",
            "{}",
            """{"version":2,"profiles":[${validProfile("one", "First")}]}""",
            """{"version":1,"profiles":{}}""",
        ).forEach { raw ->
            assertTrue("Expected empty profiles for $raw", readOnlyLibrary(raw).profiles.isEmpty())
        }
    }

    @Test
    fun `decoded profiles with unknown wire palettes are rejected in isolation`() {
        val raw = profileDocument(
            validProfile("one", "First", palette = "field"),
            validProfile("bad", "Android Preset Id", palette = "blacklight"),
            validProfile("also-bad", "Unknown", palette = "ultraviolet"),
            validProfile("two", "Second", palette = "mono"),
        )

        val library = readOnlyLibrary(raw)

        assertEquals(listOf("one", "two"), library.profiles.map { it.id })
        assertEquals(listOf("field", "mono"), library.profiles.map { it.theme.palette })
    }

    @Test
    fun `decoded themes normalize missing accents and bounded version one fields`() {
        val raw = """
            {"version":1,"profiles":[
              {"id":"one","name":"  Ops  ","theme":{
                "version":7,
                "palette":"night",
                "background":"unknown",
                "brightness":500,
                "accents":{"drone":-1,"clear":70000,"extra":123}
              }}
            ]}
        """.trimIndent()

        val profile = readOnlyLibrary(raw).profiles.single()

        assertEquals("Ops", profile.name)
        assertEquals(1, profile.theme.version)
        assertEquals("night", profile.theme.palette)
        assertEquals("dark", profile.theme.background)
        assertEquals(100, profile.theme.brightness)
        assertEquals(BadgeThemeAccentClasses.map { it.key }, profile.theme.accents.keys.toList())
        assertEquals(
            listOf(0, 0xF833, 0xF81F, 0xA81F, 0x07FF, 0xFFFF),
            BadgeThemeAccentClasses.map { profile.theme.accents.getValue(it.key) },
        )
    }

    @Test
    fun `duplicate persisted ids and names keep the first valid entries`() {
        val raw = profileDocument(
            validProfile("one", "First"),
            validProfile("one", "Different Name", palette = "night"),
            validProfile("two", " first ", palette = "mono"),
            validProfile("three", "Third", palette = "neon"),
            validProfile("", "Blank Id"),
            validProfile("four", "   "),
        )

        val library = readOnlyLibrary(raw)

        assertEquals(listOf("one", "three"), library.profiles.map { it.id })
        assertEquals(listOf("First", "Third"), library.profiles.map { it.name })
    }

    @Test
    fun `versioned json round trip is deterministic`() {
        var raw: String? = null
        val library = BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = { raw = it },
            idFactory = { "one" },
        )

        assertTrue(library.create("Field Ops", defaultBadgeTheme()))

        val expected = """{"version":1,"profiles":[{"id":"one","name":"Field Ops","theme":{"version":1,"palette":"field","background":"dark","brightness":100,"accents":{"drone":65184,"meta":63539,"tracker":63519,"flock":43039,"wifi_attack":2047,"clear":12133}}}]}"""
        assertEquals(expected, raw)
        assertEquals(
            library.profiles,
            BadgeThemeProfileLibrary({ raw }, {}).profiles,
        )
    }

    private fun inMemoryLibrary(
        vararg ids: String,
        persisted: MutableList<String> = mutableListOf(),
    ): BadgeThemeProfileLibrary {
        var raw: String? = null
        val iterator = ids.iterator()
        return BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = {
                raw = it
                persisted += it
            },
            idFactory = iterator::next,
        )
    }

    private fun readOnlyLibrary(raw: String?): BadgeThemeProfileLibrary =
        BadgeThemeProfileLibrary(
            readEncoded = { raw },
            persistEncoded = { error("loading must not persist") },
        )

    private fun preset(id: String): BadgeTheme = badgeThemePresetById(id)!!.theme

    private fun profileDocument(vararg profiles: String): String =
        """{"version":1,"profiles":[${profiles.joinToString(",")}]}"""

    private fun validProfile(
        id: String,
        name: String,
        palette: String = "field",
    ): String = """
        {"id":"$id","name":"$name","theme":{
          "version":1,
          "palette":"$palette",
          "background":"dark",
          "brightness":100,
          "accents":{"drone":65184,"meta":63539,"tracker":63519,"flock":43039,"wifi_attack":2047,"clear":12133}
        }}
    """.trimIndent().replace("\n", "")
}
