# DEF CON 34 Badge Final Interface and Easter Egg Design

## Summary

This release gives the DEF CON 34 Friend or Foe badge a cohesive instrument-style
interface, five safe built-in color themes, named custom palettes managed from
Android, and a one-shot Purple Hat Village Easter egg. It deliberately preserves
the existing four display lanes and their selection behavior.

The Easter egg appears when the badge receives either a precisely matching BLE
Remote ID observation, a matching Wi-Fi SSID observation, or a manual test press
on the currently diagnostic-only first button. Any physical badge button dismisses
the overlay, and the badge will not show it again until reboot.

Network-to-uplink-to-scanner firmware distribution is the next bounded release.
It is not mixed into this visual release because updater failures have a much
larger recovery cost than a display failure.

## Goals

1. Make the badge read as one intentional instrument panel rather than unrelated
   rows of text.
2. Preserve the existing four-lane information contract and scanner-health strip.
3. Add one-tap `Field`, `Blacklight`, `Inferno`, `Ghostline`, and `Obsidian Gold`
   themes in the Android badge controls.
4. Let Android users create, name, edit, save, apply, and delete custom badge
   palette profiles.
5. Store the active theme on the badge so it survives badge reboots.
6. Add the exact, one-shot DEF CON 34 PHV Easter egg behavior described below.
7. Produce clean, version-aligned badge scanner and badge uplink firmware artifacts
   and verify every published badge download before calling the release shipped.

## Non-Goals

1. Do not change detection, ranking, deduplication, paging, investigation, or
   suppression policy.
2. Do not replace the four-lane screen with a new navigation model.
3. Do not persist the Easter egg latch in NVS; reboot intentionally rearms it.
4. Do not make the Easter egg a threat, detection class, or normal focus item.
5. Do not add network OTA, automatic scanner flashing, or badge-to-badge firmware
   transport in this release.
6. Do not require new display hardware or a different screen orientation.

## Confirmed Display Compatibility Contract

The current 128 x 160 ST7735 badge UI has four focusable lanes:

1. Global concern number one.
2. Global concern number two, deduplicated from lane one.
3. The BLE scanner billboard.
4. The Wi-Fi scanner billboard.

The scanner-health strip remains fixed at the bottom of the display. The existing
focus model remains capped at four entries. This release may change drawing and
color lookup inside those rectangles, but it must not change:

- which candidates enter a lane;
- the ranking or deduplication rules;
- lane order, row boundaries, or the bottom health-strip boundary;
- focus order, page position, detail mode, or button-2 gestures;
- scanner freshness, safety floors, or threat meaning.

Regression tests must treat those behaviors as a frozen interface contract.

The clean `origin/main` baseline passes all 374 native tests and both badge
builds. Both linker reports already show IRAM at 16,384 / 16,384 bytes, so this
feature must not add `IRAM_ATTR` code or move display helpers into IRAM. The
converted logo and static theme tables remain flash-resident. The badge uplink
baseline uses 1,457,709 / 2,097,152 app bytes (69.5 percent); build gates record
the post-change delta rather than relying only on a successful link.

## Visual Direction: Instrument Stack

The selected visual grammar is `Instrument Stack`, the lowest-risk of the three
concept directions. Each existing row becomes a compact instrument readout with:

- a thin lane rail and stable lane token;
- a clear title/detail hierarchy using the current text content;
- a compact state chip derived from the row's existing state;
- a small signal or heat meter derived only from evidence already available to
  the renderer;
- consistent panel edges, spacing, and chrome from the active theme.

The decoration is subordinate to the data. If a state chip or meter would remove
space required by an existing safety label, the safety label wins. Empty and
scanner-down states retain their current meaning and remain immediately legible.

No animation is required for the primary lanes. This keeps redraw cost, flash
size, and timing risk low during the final hardware push.

## Theme Model

### Built-In Themes

The Android app exposes these five primary presets:

| Android preset ID | Label | Badge base palette | Intent |
| --- | --- | --- | --- |
| `field` | Field | `field` | Existing safe green/cyan field default |
| `blacklight` | Blacklight | `neon` | Purple, ultraviolet, cyan, and hot-pink PHV energy |
| `inferno` | Inferno | `night` | Deep red, orange, and warning gold |
| `ghostline` | Ghostline | `mono` | Near-black, ice cyan, white, and spectral green |
| `obsidian_gold` | Obsidian Gold | `night` | Black, charcoal, warm gold, and restrained cream |

The Android preset ID and display label are app concepts. The badge wire payload
continues using only the already valid version-1 palette tokens `field`, `night`,
`neon`, and `mono`. This means the new Android app can apply every new look to an
older badge without sending an unknown palette token that would reject the whole
theme. Exact background, brightness, and accent values distinguish presets that
share a base palette. Android recognizes a read-back preset by its complete
normalized payload fingerprint, not by the base palette token alone.

Selecting a built-in preset updates its background style, brightness, and all six
semantic accents as one atomic draft. The Android preview changes immediately;
the badge changes only when the user presses Apply.

### Custom Profiles

A custom profile contains:

- a user-visible name stored on the Android device;
- one of the four compatible base palettes for interface chrome;
- one of the supported background styles (`dark`, `dim`, or `scanline`);
- brightness from 25 through 100 percent;
- arbitrary colors for `drone`, `meta`, `tracker`, `flock`, `wifi_attack`, and
  `clear`.

Android edits colors as RGB/hex values and converts them deterministically to the
badge's RGB565 format. A profile editor provides a color preview, hex entry, and
RGB controls without adding a third-party color-picker dependency. Invalid hex
input cannot be saved or applied.

Named profiles live in a dedicated Android preferences store and can be reused
across badges. Only the active theme payload is sent to and persisted by a badge;
profile names and the user's profile library do not consume badge NVS.

The wire schema and accepted tokens stay compatible with the existing version-1
`BadgeTheme` JSON. Custom profile names and Android preset IDs are never placed in
the badge `palette` field. Firmware applies base-palette interface chrome while
using the six explicit semantic accents for detection classes. Existing version-1
stored themes load without migration loss, and old firmware continues accepting
new app-generated themes.

### Safety and Contrast

Themes must never hide operational state. The renderer enforces minimum luminance
and contrast for primary text, scanner-down text, selection outlines, and critical
status markers. A custom color that falls below a safety floor is adjusted only
for the protected element at draw time; the stored user color is not silently
rewritten.

The Android preview shows the effective RGB565 color, including quantization, so
what the user approves matches the badge as closely as the display permits.

## Easter Egg Trigger Contract

The Easter egg has three OR-connected trigger sources.

### 1. BLE Remote ID Trigger

A fully assembled OpenDroneID observation triggers only when all of these fields
are present in the same tracked Remote ID identity:

1. Basic ID value is exactly the lowercase ASCII string `fof-michagain` after
   trimming protocol padding. The misspelling is intentional; case changes and
   substrings do not match.
2. Latitude is exactly `42.4347200` degrees at OpenDroneID's encoded `1e-7`
   degree precision.
3. Longitude is exactly `-83.9850000` degrees at that same encoded precision.
4. Geodetic altitude is exactly `666.0` meters. OpenDroneID represents altitude
   in 0.5-meter increments, so no floating-point tolerance is needed after
   normalization to half-meter units.

The matcher operates on normalized protocol units rather than raw floating-point
epsilon comparisons. A missing Basic ID, stale fields from another Remote ID
identity, pressure altitude substituted for geodetic altitude, or any one-unit
coordinate/altitude difference is a negative match.

### 2. Wi-Fi SSID Trigger

An observed beacon or probe identity triggers when its complete SSID is exactly
the lowercase ASCII string `fof-goblue` after normal SSID length handling. Case
changes, prefixes, suffixes, and embedded matches do not trigger.

### 3. Manual Hardware Trigger

A debounced short press of badge button 1 triggers the Easter egg for hardware
testing. Button 1 is currently diagnostic-only, so this adds no conflict with the
working button-2 single, double, long, focus, detail, investigation, or pairing
gestures.

## Easter Egg Transport and State

BLE and Wi-Fi matching happens on the scanner that owns the parsed observation.
The scanner emits a small explicit UART event to the badge uplink. The event is
not placed in the detection queue as a threat and cannot occupy or reorder a
display lane. The uplink accepts the event from either attached scanner and feeds
one local Easter egg state machine.

The uplink state machine has three states:

1. `ARMED` after every boot.
2. `VISIBLE` after the first valid remote, SSID, or manual trigger.
3. `DISMISSED` after any physical button press while visible.

`VISIBLE` and `DISMISSED` both reject every later trigger. The state is RAM-only.
There is no timeout. While the overlay is visible, the first debounced press of
either badge button dismisses it immediately and consumes that press through its
release, preventing the same gesture from also navigating, investigating, or
opening another overlay.

Malformed or unknown UART events are ignored. Repeated scanner events are
idempotent. The event contains a source enum for diagnostics but no remotely
supplied display text.

## Easter Egg Presentation

The Easter egg is a full-screen overlay above the frozen normal display. It uses
a dedicated purple/black `Blacklight` treatment regardless of the active normal
theme so every badge presents the joke consistently.

The screen uses an embedded, display-optimized derivative of the official Wall
of Sheep logo sourced from:

`https://www.phvillage.io/wp-content/uploads/2022/10/wall-of-sheep_1-1.png`

The converted asset retains a source/attribution comment in the firmware tree and
is sized and color-quantized offline to avoid PNG decoding or heap allocation on
the badge.

The three visible text lines are exactly:

```text
Welcome to Hell
Just Kidding
Defcon 34 FoF
```

The composition uses a purple edge frame, ultraviolet glow bands, the sheep mark
as the focal point, high-contrast type, and small instrument ticks. It must remain
legible on the physical ST7735 at normal viewing distance; visual noise cannot
obscure the three required lines.

## Android Interface

`Badge Appearance` becomes a cohesive editor rather than a row of unrelated
controls:

1. A scaled four-lane badge preview at the top uses the same semantic colors and
   lane grammar as firmware.
2. A horizontally scrollable preset strip provides the five built-in themes.
3. `Interface` controls group background style and brightness.
4. `Signal Colors` groups the six semantic accent editors.
5. `Saved Profiles` supports create/name, replace, apply, rename, and delete.
6. A sticky action row keeps Apply, Reset, and Refresh clear.

The same component remains shared by the existing List and Privacy screens.
Profile storage and RGB565 conversion live outside Compose so JVM tests can cover
them without UI instrumentation.

## Error and Lifecycle Behavior

- An invalid or partial theme received by firmware is rejected atomically; the
  prior active theme remains in RAM and NVS.
- If a stored theme is invalid at boot, firmware logs the failure and uses Field.
- An Android custom-profile decode failure skips only that profile and preserves
  the rest of the library.
- Applying a badge theme without a connected badge leaves the saved Android
  profile intact and surfaces the existing transport failure state.
- Theme rendering performs no per-frame allocation.
- Easter egg matching is bounded, does not retain arbitrary transmitter strings,
  and never blocks scanner processing.
- Reboot during the Easter egg clears the RAM latch and returns to `ARMED`, as
  required.

## Release Hardening Included Here

The latest baseline still has two fail-open release problems that must be fixed
before this visual release is called shippable:

1. The `uplink-s3-fof_badge` build currently reports the production runtime
   identity `uplink-s3`. The badge variant must report
   `uplink-s3-fof_badge` consistently through serial identity, HTTP status/upload,
   firmware catalog lookup, release metadata, and tests, while the production
   uplink continues reporting `uplink-s3`.
2. The Pages flasher deploy job currently runs even after failed or canceled
   artifact production and tolerates missing downloads. Deployment must become
   fail-closed: require the build job, require every expected badge artifact,
   validate manifests and referenced files in a staging tree, and publish that
   complete tree only after validation succeeds.

The previously observed missing live badge binaries are no longer missing, and
backend catalog code now reads the embedded firmware version rather than labeling
an image with the release tag. Both corrected behaviors receive regression gates;
they are not reopened or reimplemented.

## Verification

### Native ESP Tests

1. Remote ID exact positive fixture.
2. Negative fixtures for wrong/missing ID, one coordinate unit high/low, wrong or
   missing altitude, pressure-only altitude, and mixed identity state.
3. SSID exact positive fixture plus prefix, suffix, embedded, and unrelated
   negatives.
4. UART event serialization/parsing, malformed input, repeated event idempotence,
   and source diagnostics.
5. State transitions for remote, SSID, and button-1 triggers; either-button
   dismissal; consumed dismissal gesture; no retrigger until reinitialization.
6. Theme base-palette values, version-1 compatibility, explicit accent parsing,
   invalid-theme atomicity, RGB565 brightness, and safety contrast floors.
7. Frozen four-lane candidate, rank, deduplication, focus, paging, button-2, and
   bottom-strip regression fixtures.

### Android JVM Tests

1. Built-in preset IDs and exact payloads.
2. RGB888/hex to RGB565 conversion and preview round trips.
3. Custom profile create, rename, replace, delete, stable ordering, malformed
   profile isolation, and persistence codec behavior.
4. Badge theme command serialization, complete-payload preset recognition, and
   badge status parsing for built-in, custom, and legacy themes.

### Builds and Static Gates

1. Full Android JVM tests and debug APK assembly.
2. Full native ESP suite.
3. Clean PlatformIO builds for `scanner-s3-combo-fof_badge` and
   `uplink-s3-fof_badge`.
4. Existing production scanner and uplink builds as regression gates when the
   release workflow packages all firmware families.
5. `git diff --check`, version-length check, embedded-version audit, manifest
   parity test, and secret-pattern scan.
6. Badge-vs-production runtime identity fixtures and a workflow fixture or script
   proving that a missing artifact aborts flasher publication.

### Physical and Release Gates

1. On real hardware, trigger with button 1, dismiss with each button separately,
   verify button 2 afterward, reboot, and trigger again.
2. Transmit the exact BLE Remote ID fixture and every negative boundary fixture.
3. Broadcast the exact SSID and negative variants.
4. Inspect all five themes plus one custom palette on the actual ST7735 for text
   contrast, scanner-down visibility, lane stability, and redraw artifacts.
5. Do not claim physical verification until Charles and the badge hardware are
   available; record that gate explicitly if the software build finishes first.
6. Publish firmware only after badge scanner/uplink assets exist, their embedded
   versions and target identities match the catalog/manifests, flasher URLs return
   successful responses, and downloaded hashes match the attached release assets.

## Next Release: Network-to-UART Firmware Distribution

The next project hardens the existing bootstrap-once update chain:

```text
laptop USB/UART -> flash badge uplink -> stage verified scanner image
                -> badge uplink serial queue -> attached BLE scanner
                                             -> attached Wi-Fi scanner
```

The repository already downloads and caches scanner firmware, accepts scanner
firmware checks, and automatically relays a staged image over UART. The next
release makes that path dependable rather than creating a second transport. It
must start auto-check after runtime network enablement, prompt both scanners after
a cache refresh, serialize the two relay jobs, compare ordered versions instead
of mere inequality, read the embedded version from network-uploaded custom images
instead of labeling them `custom`, enforce the advertised SHA-256, and prove
convergence after reboot and rollback.

The default laptop flow performs one direct upload connection to the badge
uplink. It does not invoke a separate relay command for each healthy scanner.
After staging succeeds, the uplink prompts both attached scanners, compares the
embedded numeric `major.minor.patch` version, and serially flashes only scanners
whose version is strictly older. Equal, newer, unknown, cross-target, or invalid
versions are never automatically flashed. Explicit same-version/direct-scanner
flashing remains recovery-only.

The initial interpretation is that each badge uplink automatically updates its
own two attached scanner radios after comparing their reported target and embedded
version. That is different from one physical badge wirelessly flashing other
physical badges. The latter would require a separate authenticated distribution,
anti-rollback, power-loss recovery, bandwidth, and fleet-coordination design.

The updater design must include target identity, signed or cryptographically
verified manifests, hash and size checks, rollback, power-loss behavior, staged
rollout, scanner readiness handshakes, retransmission, version convergence proof,
and a direct-flash recovery path before automatic updates are enabled.
