# Android Badge Themes and Custom Profiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Android badge controls five one-tap themes, an accurate four-lane preview, arbitrary RGB/hex semantic colors, and reusable named custom profiles without changing the badge version-1 wire contract.

**Architecture:** Pure Kotlin preset/color/profile contracts live outside Compose and are fully JVM-testable. A singleton SharedPreferences-backed profile store is injected into both badge ViewModels. The shared `BadgeAppearanceSection` composes the same preview/editor in List and Privacy while existing Apply remains the only action that mutates a connected badge.

**Tech Stack:** Kotlin, Jetpack Compose Material 3, StateFlow, Hilt, Gson, SharedPreferences, JUnit 4.

## Global Constraints

- Badge wire palette tokens remain exactly `field`, `night`, `neon`, and `mono`.
- Android-only preset IDs are `field`, `blacklight`, `inferno`, `ghostline`, and `obsidian_gold`; never serialize the latter four as badge palette tokens.
- Preset recognition compares a normalized complete payload in fixed accent order, not `themeHash` or palette alone.
- Accent keys remain `drone`, `meta`, `tracker`, `flock`, `wifi_attack`, and `clear`.
- Custom profile names are trimmed, 1 through 32 characters, and unique case-insensitively.
- Android previews the effective RGB565 value after quantization.
- Badge changes only when Apply is pressed; selecting/editing a preset updates the local draft and preview.
- Reset does not delete saved profiles.
- Add no third-party dependency.

---

### Task 1: Preset Catalog and RGB565 Codec

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemePresets.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemeColorCodec.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemePresetsTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemeColorCodecTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemeWireTest.kt`

**Interfaces:**
- Produces: `BadgeThemePreset`, `BadgeThemePresets`, `badgeThemePresetById`, `recognizeBadgeThemePreset`, `BadgeTheme.normalizedV1`, `BadgeTheme.payloadFingerprint`, `Rgb888`, and `BadgeThemeColorCodec`.
- Consumes: existing `BadgeTheme`, `BadgeThemeAccentClasses`, and version-1 JSON serialization.

- [ ] **Step 1: Write failing preset and codec tests**

```kotlin
@Test fun `five presets use only legacy wire palette tokens`() {
    assertEquals(
        listOf("field", "blacklight", "inferno", "ghostline", "obsidian_gold"),
        BadgeThemePresets.map { it.id },
    )
    assertTrue(BadgeThemePresets.all { it.theme.palette in BadgeThemePalettes })
    assertTrue(BadgeThemePresets.all {
        it.theme.accents.keys == BadgeThemeAccentClasses.map { info -> info.key }.toSet()
    })
}

@Test fun `blacklight exact payload is stable`() {
    val theme = badgeThemePresetById("blacklight")!!.theme
    assertEquals("neon", theme.palette)
    assertEquals("scanline", theme.background)
    assertEquals(100, theme.brightness)
    assertEquals(
        listOf(0xCFE5, 0xFA75, 0x99DA, 0xA357, 0x373F, 0xBFE9),
        BadgeThemeAccentClasses.map { theme.accents.getValue(it.key) },
    )
}

@Test fun `hex conversion previews quantized badge color`() {
    val rgb = BadgeThemeColorCodec.parseHex("#FF4CA9")!!
    assertEquals(0xFA75, BadgeThemeColorCodec.rgb888ToRgb565(rgb))
    assertEquals("#FF4CAC", BadgeThemeColorCodec.effectiveHex(0xFA75))
}
```

Add tests for optional `#`, exactly six hex digits, bounds, invalid input, quantized round-trip, complete-payload recognition, and one-field mismatch rejection.

- [ ] **Step 2: Run focused tests to verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.badge.BadgeThemePresetsTest' --tests 'com.friendorfoe.data.badge.BadgeThemeColorCodecTest' --tests 'com.friendorfoe.data.badge.BadgeThemeWireTest'
```

Expected: compilation fails because the catalog and codec do not exist.

- [ ] **Step 3: Implement the five exact preset payloads**

```kotlin
data class BadgeThemePreset(
    val id: String,
    val label: String,
    val theme: BadgeTheme,
)

internal fun BadgeTheme.payloadFingerprint(): String = buildString {
    val normalized = normalizedV1()
    append(normalized.version).append('|')
    append(normalized.palette).append('|')
    append(normalized.background).append('|')
    append(normalized.brightness)
    BadgeThemeAccentClasses.forEach {
        append('|').append(it.key).append('=').append(normalized.accents.getValue(it.key))
    }
}
```

Use these accent maps in `drone/meta/tracker/flock/wifi_attack/clear` order:

- Field: `FEA0,F833,F81F,A81F,07FF,2F65`, `field/dark/100`.
- Blacklight: `CFE5,FA75,99DA,A357,373F,BFE9`, `neon/scanline/100`.
- Inferno: `FD83,F9AB,FA44,C349,3EFE,7FEE`, `night/dark/100`.
- Ghostline: `D7EA,57B5,26AF,554F,37FB,AFEC`, `mono/scanline/100`.
- Obsidian Gold: `FE89,FA73,BC65,AC8C,7EDF,D7EC`, `night/dim/90`.

- [ ] **Step 4: Implement deterministic RGB conversion**

`parseHex` accepts an optional leading `#` and exactly six hexadecimal digits. Use truncating RGB888-to-RGB565 conversion and the same floor expansion as current `BadgeAppearanceSection` for RGB565-to-RGB888.

- [ ] **Step 5: Run focused tests to verify GREEN**

Run the Step 2 command. Expected: all three suites pass.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemePresets.kt android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemeColorCodec.kt android/app/src/test/java/com/friendorfoe/data/badge
git commit -m "android: add badge theme preset contracts"
```

---

### Task 2: Named Profile Persistence

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemeProfileStore.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemeProfileStoreTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`

**Interfaces:**
- Consumes: Task 1 normalized version-1 themes.
- Produces: singleton `BadgeThemeProfileStore.profiles: StateFlow<List<BadgeThemeProfile>>` plus create, rename, replace, and delete operations shared by both screens.

- [ ] **Step 1: Write failing pure-library tests**

```kotlin
@Test fun `profiles survive reload in stable order`() {
    var raw: String? = null
    val first = BadgeThemeProfileLibrary(
        readEncoded = { raw },
        persistEncoded = { raw = it },
        idFactory = sequenceOf("one", "two").iterator()::next,
    )
    assertTrue(first.create("Purple Ops", badgeThemePresetById("blacklight")!!.theme))
    assertTrue(first.create("Gold Ops", badgeThemePresetById("obsidian_gold")!!.theme))

    val reloaded = BadgeThemeProfileLibrary({ raw }, { raw = it })
    assertEquals(listOf("Purple Ops", "Gold Ops"), reloaded.profiles.map { it.name })
}

@Test fun `blank and case-insensitive duplicate names are rejected`() {
    val library = inMemoryLibrary()
    assertFalse(library.create("   ", defaultBadgeTheme()))
    assertTrue(library.create("Field Team", defaultBadgeTheme()))
    assertFalse(library.create("field team", defaultBadgeTheme()))
}
```

Add tests for 32/33-character boundaries, rename preserving ID/order, replace preserving name/ID/order, delete, malformed-entry isolation, unknown palette rejection, missing accent normalization, and persistence only after successful mutation.

- [ ] **Step 2: Run the focused profile test to verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.badge.BadgeThemeProfileStoreTest'
```

Expected: missing profile contracts.

- [ ] **Step 3: Implement a pure library plus Android adapter**

```kotlin
data class BadgeThemeProfile(val id: String, val name: String, val theme: BadgeTheme)

internal class BadgeThemeProfileLibrary(
    private val readEncoded: () -> String?,
    private val persistEncoded: (String) -> Unit,
    private val idFactory: () -> String = { UUID.randomUUID().toString() },
) {
    var profiles: List<BadgeThemeProfile> = BadgeThemeProfileCodec.decode(readEncoded())
        private set
    fun create(name: String, theme: BadgeTheme): Boolean
    fun rename(id: String, name: String): Boolean
    fun replace(id: String, theme: BadgeTheme): Boolean
    fun delete(id: String): Boolean
}
```

Use dedicated SharedPreferences file `fof_badge_theme_profiles` and key `profiles_v1`. The top-level JSON is `{"version":1,"profiles":[...]}`. Decode entries individually so one malformed profile does not discard the library.

Annotate the adapter `@Singleton` with an `@Inject` constructor and `@ApplicationContext`; Hilt needs no module for the concrete type.

- [ ] **Step 4: Inject and expose the shared store**

Add `BadgeThemeProfileStore` to both ViewModel constructors. Expose `badgeThemeProfiles` and small create/rename/replace/delete delegators. Do not move connected-badge transport into the profile store.

- [ ] **Step 5: Run focused and full JVM tests**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.badge.BadgeTheme*Test'
./gradlew testDebugUnitTest
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/data/badge/BadgeThemeProfileStore.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemeProfileStoreTest.kt
git commit -m "android: persist named badge palettes"
```

---

### Task 3: Cohesive Appearance Editor and Four-Lane Preview

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeThemePreview.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeThemeColorEditorDialog.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeAppearanceSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlStatusParserTest.kt`

**Interfaces:**
- Consumes: Task 1 presets/codec and Task 2 profile StateFlow/operations.
- Produces: one shared appearance editor used unchanged by List and Privacy.

- [ ] **Step 1: Extend parser regression tests before UI work**

Add full six-accent readback fixtures for each base wire palette, partial legacy theme fallback, and an arbitrary custom payload that is intentionally not recognized as a built-in preset.

- [ ] **Step 2: Run parser and theme tests**

Expected: existing tests pass and new fixtures fail only where parsing/fingerprinting needs adjustment.

- [ ] **Step 3: Build the static four-lane preview**

The preview draws two global rows, a BLE row, a Wi-Fi row, and a bottom health strip using draft theme colors. It is presentation-only: do not import detection ranking or create a second lane policy.

- [ ] **Step 4: Build the dependency-free color editor**

Use a Material 3 dialog with a color preview, hex field, and R/G/B controls. Save is disabled until `parseHex` succeeds. The preview label shows `effectiveHex(rgb565)` so quantization is visible.

- [ ] **Step 5: Restructure the shared appearance section**

Extend its parameters with the profile list and profile callbacks. Compose, in order: four-lane preview, horizontally scrollable five-preset strip, Interface group, Signal Colors group, Saved Profiles group, and Apply/Reset/Refresh action row. Preset/profile selection calls `onThemeChange`; only Apply calls existing badge transport.

- [ ] **Step 6: Wire both screens to the singleton profile flow**

Collect `badgeThemeProfiles` in List and Privacy, pass the same callbacks to `BadgeAppearanceSection`, and preserve existing `themeHash`-driven draft refresh behavior.

- [ ] **Step 7: Run complete Android verification**

```bash
cd android
./gradlew testDebugUnitTest --rerun-tasks
./gradlew assembleDebug
```

Expected: all JVM tests and debug assembly pass; record APK path, size, and SHA-256.

- [ ] **Step 8: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/badge android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/test/java/com/friendorfoe/data/badge
git commit -m "android: add the badge palette studio"
```
