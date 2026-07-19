# Android Dedicated Badge USB Page Design

**Date:** 2026-07-18

## Scope boundary

This is an Android-only change. It must not edit, rebuild, flash, or otherwise
change any ESP32 scanner or uplink firmware. The connected badge may be used as
an external USB test fixture only. Existing badge wire commands and response
frames remain the contract.

## Goals

- Give badge USB monitoring and control a dedicated top-level Android page.
- Put a custom gold three-triangle badge icon immediately after Privacy in the
  bottom navigation.
- Remove the badge connection and control panels completely from Privacy and
  List.
- Keep detections received from the badge visible in the ordinary List feed.
- Mark every badge-sourced List row with the three-triangle icon instead of a
  Remote ID, Bluetooth, or Wi-Fi source icon.
- Connect automatically when a compatible badge is attached; do not show a
  manual Connect button.
- Preserve all controls that the current firmware exposes over USB while
  keeping disruptive controls guarded.

## Non-goals

- No firmware protocol, detector, display, updater, version, or build change.
- No Android firmware upload feature. The existing policy that stages firmware
  from a laptop remains authoritative.
- No BLE badge-control path and no HTTP mutation path.
- No redesign of the badge's four-lane LCD interface.
- No changes to how phone-local or backend detections are classified.

## Navigation

Add a `Badge` route immediately after `Privacy` in the bottom navigation. The
visible order becomes:

`AR | Map | List | Privacy | Badge | History | Info`

Badge replaces the current Cal bottom-navigation slot so the bar stays at seven
items. Calibration remains reachable from Info. The Badge destination uses a
shared custom vector composed of three separated triangles. It is gold when
selected and follows normal navigation tinting when unselected.

## Dedicated Badge page

The page is an operational USB workspace, not a generic settings screen. It
uses a dedicated `BadgeControlViewModel` that depends on `BadgeUsbRepository`
and `BadgeThemeProfileStore`, rather than reusing `PrivacyViewModel` or
`ListViewModel`.

The page contains these sections in order:

1. **Badge status** — passive connection state, device identity, firmware
   version, uplink health, scanner roles/health, USB age, heap, stack, and PSRAM.
   It has Refresh but no Connect button.
2. **Live from Badge** — a bounded newest-first feed of parsed badge detections,
   command acknowledgements/errors, firmware-relay progress, and the last
   meaningful USB activity. Raw boot log spam is not retained.
3. **LCD remote** — Next, Detail/Page, and Back using the existing display-nav
   commands, plus the currently focused LCD item.
4. **Appearance** — existing preset themes, custom palettes, brightness,
   backgrounds, and saved theme profiles.
5. **Display filters** — existing per-class enablement, lane, proximity, and
   priority controls.
6. **Operations** — current USB-only mode controls, explicit per-slot scanner
   recovery commands, reboot, and bootloader. Reboot, bootloader, and scanner
   recovery require confirmation.

Command controls are enabled only for a verified USB connection. Read-only
cached status may still render while disconnected, but it must be visibly
labelled stale and cannot enable commands.

## Automatic USB lifecycle

Badge USB discovery belongs to app foreground lifecycle rather than to Privacy
or List screen lifecycle. A small app-level owner starts the singleton
`BadgeUsbRepository` when the Android app enters the foreground and stops it
when the app enters the background. Repeated starts and stops must be
idempotent so navigation cannot disconnect an active badge.

When exactly one compatible Espressif badge is attached:

- If Android already has permission, the repository verifies the firmware
  identity and connects automatically.
- If permission is missing, Android's USB permission dialog is requested
  automatically. There is no custom Connect button.
- If permission is denied, the Badge page explains that USB access was denied
  and offers a narrowly labelled `Grant USB access` retry action.
- If multiple compatible devices are attached, the page reports the ambiguity
  and sends no commands until only one eligible uplink remains. The app must not
  guess between uplink and scanner devices.

## List integration and provenance

The existing large `BadgeUsbPanel` is removed from List. `ListViewModel` no
longer owns badge controls or theme editing. It may observe the singleton badge
state read-only to produce list rows.

The List presentation model becomes a unified feed containing ordinary
`SkyObject` rows and `BadgeUsbDetection` rows. This avoids pretending a compact
USB badge event has GPS/altitude fields that the current `FOF_DET` frame does
not provide.

A badge row displays:

- the best available badge label or manufacturer;
- class and transport evidence when present;
- RSSI and confidence;
- `--` for unavailable altitude or distance;
- the gold three-triangle icon as its source marker.

The triangle marker replaces the usual Bluetooth/Remote ID/Wi-Fi icon for that
row. The underlying transport remains available in details and filtering; only
the visible source marker changes because the observation arrived from the
badge. Tapping a badge row navigates to the Badge page and requests focus on the
matching detection or entity when it is still present.

Badge USB detections need a receive timestamp and stable UI key so the combined
feed can deduplicate and order them without changing the firmware frame.

## Privacy separation

Remove all badge imports, badge state collection, badge lifecycle calls, badge
detection merging, connection UI, and control UI from `PrivacyScreen` and
`PrivacyViewModel`. Privacy continues to show only phone-local and backend
privacy detections. Badge-origin privacy findings remain available on the Badge
page and may appear in the ordinary List feed with the triangle marker.

## Error handling and safety

- Never enable a command merely because cached HTTP status exists; commands
  require a verified direct USB connection.
- Preserve firmware identity-handshake rejection for scanner MCUs and unknown
  Espressif devices.
- Bound the live activity feed to avoid unbounded Android memory growth.
- Serialize commands through the repository's existing mutex and acknowledgement
  handling.
- Confirmation dialogs identify the exact disruptive action and never combine
  reboot, bootloader, or scanner recovery into one generic confirmation.
- Leaving the Badge page must not interrupt an in-flight acknowledged command;
  the app-level USB owner remains active while the app is foregrounded.

## Testing and acceptance

Android unit tests must cover:

- the three-triangle icon and Badge route remain present beside Privacy;
- Cal remains reachable from Info and is absent from the bottom bar;
- Privacy and List no longer render or own the old badge-control panels;
- badge detections map to stable List feed items with badge provenance;
- badge-sourced rows select the triangle marker while phone/backend rows retain
  their transport icons;
- automatic USB ownership is idempotent across app foreground/navigation events;
- denied permission, ambiguous devices, disconnected state, and stale cached
  status cannot send commands;
- disruptive actions require their specific confirmation state;
- the activity feed is bounded and newest-first.

Run the Android JVM suite and build the debug APK. Then use an emulator for
navigation, empty/error states, and visual verification. Android Emulator USB
passthrough is not assumed: if the connected physical badge cannot be attached
to the emulator, report that limitation explicitly and validate real USB with a
physical Android device when one is available. Do not alter or flash the badge
to make emulator testing work.

The connected badge and Mac camera provide an additional non-firmware theme
check. If emulator USB passthrough works, apply multiple preset and custom
themes from the Android page and visually confirm the LCD response through the
camera. If passthrough is unavailable, prove the Android wire payloads in JVM
tests, send those same existing control frames from the Mac over USB, and use
camera captures to confirm the badge changes appearance. Restore the original
theme after the check. This is evidence for the existing USB contract, not a
substitute for a physical Android USB test.

## Acceptance result

The feature is accepted when Badge is a one-tap top-level destination, all
badge controls are absent from Privacy and List, compatible USB attachment is
automatic, the dedicated page exposes the proven USB controls and live feed,
badge-origin List rows use the triangle marker, Android tests/build pass, and
the ESP32 working-tree diff is byte-for-byte unchanged from the pre-work
baseline.
