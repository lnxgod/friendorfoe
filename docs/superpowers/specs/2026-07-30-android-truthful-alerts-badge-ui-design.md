# Android Truthful Alerts and Badge UI Design

**Date:** 2026-07-30
**Implementation baseline:** `origin/main` at or after `7b969a6`
**Affected surface:** Android app only

## Goal

Make the Android app truthful and calmer in three places:

1. Stop presenting passive AirPods/Apple BLE activity as evidence that someone is listening.
2. Open the map at the user's location without visibly animating across the map.
3. Limit helicopter notifications to a configurable radius that defaults to 25 statute miles.

Also reorganize the existing Badge Control interface so its current controls are easier to find and use without changing badge control, transport, domain logic, backend behavior, or firmware.

## Hard Scope Boundaries

- Do not edit, build, flash, upload, or otherwise change badge, scanner, uplink, or other ESP32 firmware.
- Do not change the badge USB protocol, command payloads, transport ownership, identity verification, command allowlist, repository lifecycle, or ViewModel behavior.
- Do not change backend code or API contracts.
- Do not add multi-badge targeting, badge selection, "apply to both," firmware update, bootloader, recovery, flashing, or OTA controls.
- Do not change ADS-B fetch radius, map/list visibility, drone alerts, or the existing 15-mile military and police/emergency alert policies.
- Local theme profiles remain Android-local and shared for sequential use with badges exactly as they are today.

The two attached badges are validation targets only. Their freshly flashed firmware is authoritative and remains untouched.

## Selected Approach

Use four isolated Android changes that share no new domain abstraction:

- Normalize AirPods-related privacy presentation to informational Apple activity.
- Give the map an explicit first-center state.
- Add a helicopter-only notification radius preference.
- Split Badge Control's existing Compose content into four operator-oriented tabs while retaining the same root state and callbacks.

This is preferred over a copy-only patch, which would leave incorrect threat counting and notifications in place, and over a protocol/provider overhaul, which would exceed the Android-only scope.

## 1. Truthful AirPods Presentation

### Current Problem

The Android local scanner combines an AirPods-related Apple Nearby Info flag with a coarse Apple activity value and promotes the result to:

- `Possible Remote Listening`
- threat level 2
- privacy threat count
- automatic category expansion
- a privacy notification

Passive BLE does not reveal Live Listen activation, microphone state, audio routing, audio content, ownership, or intent. Backend and badge-origin rows can also carry similarly aggressive wording into Android.

### Desired Behavior

AirPods-related BLE evidence is informational, not a listening threat:

- Title: `AirPods connection/activity nearby`
- Detail: `An Apple device reports connected AirPods and media, call, or video activity.`
- Persistent qualifier: `Live Listen and microphone use cannot be determined from BLE.`
- Category: `APPLE_CONTINUITY`
- Threat level: the category's existing informational level
- No privacy-threat count contribution
- No privacy notification

The wording must use `connected` or `connection signal`, never `AirPods in`, `listening`, `remote listening`, or language implying microphone use.

### Android Data Flow

1. The local Apple matcher stops producing `REMOTE_LISTENING` for the AirPods/activity correlation.
2. The same observation may produce an informational `Apple Continuity AirPods` detection with neutral evidence text.
3. Backend-origin detections are normalized in Android when, and only when, Apple/AirPods evidence accompanies a `REMOTE_LISTENING` kind or listening-oriented label.
4. Badge-origin activity shown by Android is normalized at the presentation boundary when its title/detail identifies the same AirPods heuristic.
5. Generic non-Apple listening categories are not silently rewritten.

Normalization is deterministic and implemented as pure functions so local, backend, and badge presentation can share tested wording without changing their transports.

### Error and Edge Behavior

- Missing or ambiguous Apple activity is shown as `Apple activity state unavailable`.
- An Apple manufacturer ID alone is not enough to claim AirPods.
- Rotating MAC addresses do not raise the normalized observation back into a threat category.
- Existing ignored-identity behavior remains unchanged.

## 2. Map Initial Position

### Current Problem

The map is displayed before a valid user position exists. Its first valid location then calls `animateTo`, producing a visible trip from the default center. A stale last-known fix can cause another movement.

### Desired Behavior

- While no valid user position exists, show a restrained `Getting your location…` state instead of a map centered at `(0, 0)`.
- The first valid user position is applied synchronously with `setCenter`; it never animates.
- Subsequent automatic recentering retains the existing distance threshold, compass-follow behavior, and user-pan timeout.
- User gestures continue to suppress automatic recentering exactly as they do now.

### Component Boundary

`MapOverlayController` owns a retained `hasInitialCenter` flag. A small pure policy function decides among:

- `NONE`
- `SNAP`
- `ANIMATE`

`MapViewScreen` prevents the uncentered map from becoming visible before the first valid position. No repository polling, location-update interval, map overlays, tracks, or remote search behavior changes.

### Error and Edge Behavior

- Permission or provider failures leave the location state visible rather than showing an unrelated part of the world.
- A later valid fix initializes the map immediately.
- Resuming the same retained map does not repeat initial centering.
- Recreating the screen performs one new snap, never a cross-map animation.

## 3. Configurable Helicopter Alert Radius

### Desired Behavior

- Persist a helicopter notification radius in Android preferences.
- Default: `25` statute miles.
- Allowed UI range: `5` through `50` statute miles in 5-mile increments.
- The setting applies only to helicopter notifications.
- A helicopter must have a known distance and be within the configured radius.
- Objects exactly on the boundary are eligible.
- Helicopters outside the radius or with unknown distance do not notify.

The setting does not alter fetching, map/list rendering, history, enrichment, other aircraft categories, or backend behavior.

### Data Flow

1. `DetectionPrefs` persists `helicopterAlertRadiusMiles`, returning 25 when unset and coercing invalid stored values to the supported range.
2. `AboutViewModel` exposes the value and a setter.
3. `AboutScreen` places a compact stepped radius control directly below `Helicopter Alerts`, showing the current value in statute miles.
4. `SkyAlertSettings` carries the radius.
5. `SkyAlertNotifier` and `PrivacyViewModel` read the same persisted value.
6. `SkyAlertPolicy` applies the value only in the helicopter branch.

The supporting copy becomes `Notify for helicopters within 25 mi` and updates with the chosen value.

## 4. Badge Control UI-Only Redesign

### Invariant State and Actions

`BadgeControlViewModel`, `BadgeUsbRepository`, `BadgeControlTransportPolicy`, command JSON, connection ownership, and firmware-facing types remain unchanged.

The screen continues to observe:

- `BadgeUsbState`
- local theme profiles
- the current BLE investigation

The screen continues to own:

- theme draft
- display-policy draft
- selected interface tab
- pending reboot confirmation
- selected entity

Draft state is held above the tab content so switching tabs cannot reset edits.

Every current callback is reused directly. No callback is invented, combined, broadened, or retargeted.

### Persistent Header

Every tab begins below one compact status header containing:

- badge triangle mark
- connection state and transport
- active hardware-ID suffix when available
- firmware version
- current network mode
- refresh action
- stale/read-only/ambiguous-device state when applicable

The header displays the currently verified badge identity but is not a badge selector. When two eligible devices create transport ambiguity, the existing fail-closed behavior and disabled command surface remain visible.

### Tabs

#### Overview

- LCD snapshot and the existing `NEXT`, `DETAIL`, `PAGE`, and `BACK` actions
- compact scanner/system-health summary
- connection guidance and permission action when needed

#### Display

- live badge theme preview
- existing complete-theme presets
- base palette, background, brightness, and accent editors
- saved local profiles
- display-density presets and all existing per-class display-policy fields
- explicit draft language
- unambiguous `Reset draft`, `Refresh from badge`, and `Apply changes` actions

Appearance and display policy retain separate existing apply/reset callbacks where required. Visual proximity does not merge their command semantics.

#### Activity

- bounded live badge feed
- detected entities
- entity detail
- conditional investigation action
- active investigation result and cancel action

Focused entries remain promoted, and all existing feed bounds and identity behavior remain intact.

#### Advanced

- existing network modes: `local_ap`, `backend`, and `usb_only`
- progressively disclosed detailed diagnostics
- the existing reset-theme action with wording that distinguishes it from resetting a local draft
- isolated reboot danger zone using the existing confirmation reducer

No firmware, bootloader, recovery, flashing, upload, or OTA action is shown.

### Connection and Safety Presentation

- Mutations remain enabled only for verified USB `CONNECTED`.
- Read-only transports may refresh only where the existing transport policy allows it.
- Losing verified transport closes a pending reboot confirmation through the existing reducer.
- Long identity, version, and error strings wrap or ellipsize without hiding status.
- Selected segmented controls expose proper selected semantics and accessible labels.

## Visual Direction

The approved concept uses a calm, compact operations-console layout:

- background `#0B1117`
- surfaces `#101820` and `#263241`
- primary cyan `#7DD3FC`
- success green `#86EFAC`
- badge gold `#FBBF24`
- restrained danger red `#F87171`

It follows the app's existing Material 3 typography and bottom navigation. It avoids cyberpunk decoration, oversized cards, nested scrolling regions, excess pills, and walls of always-expanded diagnostics.

## Testing Strategy

All behavior changes follow test-first development.

### JVM Unit Tests

- AirPods activity does not produce `REMOTE_LISTENING`.
- Informational AirPods presentation contains the explicit BLE limitation.
- Backend Apple/AirPods listening labels normalize to `APPLE_CONTINUITY`.
- Unrelated non-Apple listening categories are preserved.
- Badge AirPods activity copy is neutralized only when Apple/AirPods evidence is present.
- First valid map center chooses `SNAP`.
- Later distant automatic recenter chooses `ANIMATE`.
- User panning chooses `NONE`.
- Default helicopter radius is 25 statute miles.
- Helicopters just inside, exactly on, and just outside the configured boundary behave correctly.
- Unknown-distance helicopters do not alert.
- Drone, military, and police/emergency policy remains unchanged.
- Badge tab switching preserves root-owned drafts.
- Existing badge command-gating and reboot-confirmation tests remain unchanged and pass.

### Emulator Validation

Use `Pixel8_API35` for:

- map startup from a controlled emulator location with no visible cross-map animation
- helicopter radius persistence across app restart
- disconnected, permission-needed, and stale badge-screen states
- all four tabs at normal and large font scales
- Display draft retention across tab changes
- dialogs, scrolling, selected semantics, and accessibility labels
- screenshots at phone-sized viewports

### Physical Badge Validation

Firmware remains untouched.

With the two already-flashed badges attached, validate:

- the existing ambiguous-device state fails closed
- identity/status copy remains readable
- mutation controls remain disabled when ownership is ambiguous

If a single verified badge is available without reflashing or changing firmware, limit commands to the Android screen's existing safe protocol and restore any temporary display setting. No firmware or protocol command is added.

### Build and Regression Verification

- targeted JVM tests for each new policy/helper
- full Android JVM unit-test suite
- `assembleDebug`
- emulator interaction and screenshots
- source audit proving no file under `esp32/` or `backend/` changed
- diff audit proving badge repository, transport, ViewModel, command payloads, and firmware-facing types did not change

## Completion Criteria

The work is complete only when:

1. Android no longer presents AirPods activity as listening, a privacy threat, or a notification from any Android-visible source.
2. The map first appears at a valid user location without a visible travel animation.
3. Helicopter alerts default to 25 statute miles and honor the persisted configurable radius.
4. Badge Control matches the approved four-tab information architecture.
5. Badge domain/control logic and firmware remain byte-for-byte untouched.
6. The visual concept has been delivered.
7. Targeted tests, the full Android JVM suite, the debug build, emulator checks, and safe physical-device checks provide current passing evidence.
