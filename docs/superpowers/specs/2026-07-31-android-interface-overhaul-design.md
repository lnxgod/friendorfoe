# Android Interface Overhaul Design

**Date:** 2026-07-31

**Status:** Proposed; awaiting written-spec review

**Affected surface:** `android/` only

**Supersedes:** `2026-07-30-android-truthful-alerts-badge-ui-design.md` for Android alert, navigation, badge, and interface work

## Goal

Finish the Android app as a dependable, understandable product without changing the backend or any firmware. Preserve seven one-tap top-level destinations while making every screen easier to scan, making controls truthful about what they can do, and correcting high-impact behavior discovered during a source and emulator audit.

The seven destinations are:

1. AR
2. Map
3. List
4. Privacy
5. Badge
6. History
7. Info

The fifth triangle/tune destination is Badge configuration. Triangulation Calibration is not a top-level destination; it moves to `Info > Advanced` and remains de-emphasized while its backend is unavailable.

## Hard Scope Boundaries

- Change Android code, Android resources, Android tests, and Android documentation only.
- Do not edit, build, flash, upload, or otherwise change ESP32 badge, scanner, BLE-scanner, or uplink firmware.
- Do not change backend code, schemas, endpoints, deployment, or configuration.
- Do not invent firmware capabilities or claim a command succeeded without Android-visible evidence.
- Do not add an eighth top-level destination.
- Do not turn Privacy into a settings or badge-control screen.
- Do not rewrite the entire Android application architecture. Improve boundaries that directly serve this overhaul.
- Preserve existing detection inputs and transport integrations unless a specific audited behavior is misleading, unsafe, or nonfunctional.

## Selected Delivery Approach

Combine a staged correctness overhaul with a substantial visual cleanup:

1. Fix trust and correctness blockers first.
2. Establish the seven-destination shell and clear route ownership.
3. Build dedicated Privacy and Badge experiences.
4. Clean up the remaining routes using shared components and state conventions.
5. Add proportional unit and Compose coverage, then verify a debug APK in an emulator.

This is preferred over a visual-only patch, which would leave misleading behavior behind, and over a full rewrite, which would delay a usable Android release and increase regression risk.

## Product Principles

### One destination, one job

Top-level destinations do not embed another destination's primary controls. List shows sky objects, Privacy shows current privacy findings, Badge configures the badge, and Info owns settings/help/about.

### State before controls

Users see whether a source or device is live, paused, stale, unavailable, unsupported, permission-blocked, or failed before they are offered actions.

### Evidence before claims

BLE presence, signal, or vendor activity flags must not be presented as proof of intent, microphone use, listening, spying, or camera activity. Uncertain classifications remain visibly uncertain.

### Explicit mutation

Saving photos, applying badge configuration, rebooting hardware, or entering bootloader mode requires an explicit labeled action. Destructive or device-mutating operations require confirmation.

### Progressive disclosure

The first screen answers the common question. Diagnostics, raw identifiers, calibration, and recovery tools live behind secondary routes or Advanced sections.

## Navigation and Application Shell

### Stable main graph

Create a stable main navigation graph whose top-level routes are AR, Map, List, Privacy, Badge, History, and Info. Welcome is a first-run gate, not the navigation graph's permanent start anchor. Completing onboarding persists locally and enters the main graph. The selected top-level route persists in Android DataStore until app data is cleared or onboarding is reset. A missing route, renamed route, secondary route, or route unavailable in the installed version is invalid and falls back to AR. Pressing Back from a top-level destination exits the app rather than replaying previously selected tabs.

Top-level navigation uses `launchSingleTop`, saves/restores route state, and pops to the stable main-graph root rather than to a removed Welcome destination.

### Compact seven-button bar

Replace the default Material expanding selection pill with a compact seven-slot bar:

- all seven icons remain visible;
- normal text sizes show compact one-line labels;
- the active destination uses a thin indicator and restrained surface tint;
- the current route is always named in the screen header;
- at large font scales where seven labels cannot fit without wrapping, the bar retains the seven icons and accessible full labels while the header supplies the visible route name;
- every item has a minimum 48dp interactive area and a full TalkBack label.

The certified portrait widths are 360dp and 412dp. At 1.0x font scale the compact labels remain visible; at 1.3x and 2.0x the bar may switch to icon-only presentation while retaining all seven 48dp targets and full semantics. Widths below 336dp cannot satisfy seven simultaneous 48dp targets and are outside this release's certified layout matrix. Portrait and landscape windows narrower than 600dp use the bottom bar. A landscape window at least 600dp wide uses a compact navigation rail with the same seven actions; this is the only bottom-bar/rail breakpoint in this release.

The bar is visible on all seven top-level destinations. It is hidden on secondary workflows such as object detail, Reference Guide, EMF/IR tools, device recovery, and Calibration.

### Secondary routes

Secondary routes use a consistent top app bar with Back and a descriptive title. They never masquerade as a top-level destination. Notifications and alert taps deep-link to the relevant destination and exact item instead of launching Welcome.

## Shared State Model

### Source health

Represent each active input with an Android UI model containing:

- source identity;
- state: `LOADING`, `LIVE`, `STALE`, `PAUSED`, `PERMISSION_BLOCKED`, `UNSUPPORTED`, or `FAILED`;
- last successful update time when available;
- concise recovery action when one exists;
- cached-data age when stale data is retained.

Freshness uses an injected monotonic clock for in-session decisions and wall-clock timestamps only for displayed age or restored data. The initial policies are named constants and have boundary tests:

| Source | Stale after | Remove from Current after |
|---|---:|---:|
| Phone BLE privacy scan | 30 seconds without a matching observation | 90 seconds |
| Backend privacy feed | 15 seconds without a successful poll | 60 seconds |
| Badge USB, local AP, or debug bridge | 10 seconds without valid status | 60 seconds |
| Badge BLE | 20 seconds without valid status | 60 seconds |
| Wi-Fi anomaly analysis | 30 seconds without a successful analysis | 60 seconds |

A shorter protocol-supplied TTL only shortens the removal threshold: effective removal is the smaller of the table value and the protocol TTL, and effective stale time is the smaller of the table's stale value and effective removal. The expiry clock continues while paused. Pausing a source changes it to `PAUSED`, freezes alert eligibility, and keeps its last rows visibly cached only until their effective removal threshold. After that, the rows leave Current while the source summary remains `PAUSED`; persisted History is unchanged. Resuming with retained rows marks them `STALE` until a successful observation, while resuming after expiry returns the source to `LOADING`. Stale or paused data never produces a new high-risk notification or claims to be current.

### Finding presentation and capabilities

Every Privacy finding presented by Android carries:

- stable display identity;
- normalized category and severity;
- source;
- first/last seen data when available;
- ownership/bonded state;
- freshness;
- signal and units when meaningful;
- explicit capabilities such as `canIgnore`, `canTrack`, and `canOpenDirectionSweep`.

The UI renders only actions whose capability is true. Source-specific suppression replaces the current local-only Ignore behavior exposed on every row. Ignore is offered only when the source provides a stable identity. It persists in Android DataStore under `(source kind, stable source ID)` until the user reverses it from `Privacy > Ignored devices`; it does not survive app-data removal or reinstall. It suppresses only matching rows from that source, not unrelated backend/badge duplicates or rotating identifiers. Track/RSSI sweep is offered only for a phone-BLE identity with a live local sample stream.

### Badge connection and capabilities

The badge repository remains the transport implementation, but its lifecycle becomes app-scoped rather than started/stopped independently by List and Privacy screens. Badge UI receives a single state stream that distinguishes:

- transport open from verified live feed;
- device-reported values from Android fallback/default values;
- supported, unsupported, and unknown capabilities;
- draft configuration from last applied/acknowledged configuration;
- command pending, succeeded, failed, and timed-out states.

The initial Android capability matrix is deliberately conservative:

| Capability | Verified USB serial | Verified badge local-AP HTTP | Verified BLE | Debug bridge |
|---|:---:|:---:|:---:|:---:|
| Read status/feed | Yes | Yes | Yes | Simulated/read-only |
| Short LCD navigation (`NEXT`, `DETAIL`, `BACK`) | Yes | Yes | Yes when payload is at most negotiated MTU minus 3 bytes | No |
| Change network mode | Yes | Yes | Yes when payload is at most negotiated MTU minus 3 bytes | No |
| Apply appearance/theme | Yes | Yes | No | Preview only |
| Apply full display policy | Yes | Yes | No | Preview only |
| Reboot or enter bootloader | Yes, with confirmation | No | No | No |
| In-app firmware upload | No in this release | No | No | No |

Unknown capability is never rendered as an editable default. A control is editable only when the active transport is verified live and the matrix permits it. BLE appearance and display-policy payloads remain unavailable because the current Android path has neither chunking nor a reliable full-payload acknowledgement.

`Verified live` is transport-specific and requires a fresh, successfully parsed FoF badge status, not merely an open socket, GATT connection, HTTP 2xx response, or Espressif USB vendor ID:

- USB requires exactly one candidate with vendor ID `0x303A`, a readable serial interface, and a FoF status response with a nonblank protocol/version value inside the USB freshness window.
- Badge AP requires a valid FoF status body from the existing fixed badge-AP endpoint at `192.168.4.1` inside the AP freshness window. It is unrelated to the user-configured sensor backend.
- BLE requires the expected FoF service plus status/control characteristics and a valid parsed status value inside the BLE freshness window.
- The debug bridge is a debug-build simulator and never proves that physical hardware supports a capability.

Reboot or bootloader additionally requires the verified USB predicate above, an unambiguous single USB candidate, and a hardware-tested Android command path. If any predicate is absent, both operations stay unavailable. The support matrix may enable a physical transport only after an Android phone-plus-badge smoke test proves status plus each enabled reversible command class on that transport; an untested transport remains visibly `Unverified` and its mutating controls remain unavailable.

Command state terminology is exact:

- **Accepted:** the Android transport call/write completed without an immediate error; this is not proof of device state.
- **Acknowledged:** an explicit command response or subsequent status includes the command result/hash within 5 seconds.
- **Applied:** returned device fields match the submitted draft.
- **Verified:** the command is acknowledged and every readable submitted field is applied.

Android never retries a mutating command automatically. A 5-second acknowledgement timeout becomes a visible `Not verified` result with manual Refresh/Retry. Commands without readable post-state may report `Command accepted` or `Command acknowledged`, never `Applied` or `Verified`.

### Observable settings

Settings are exposed as observable ViewModel state. Compose rows do not keep private `remember` snapshots of plain getters. Every retained setting changes actual runtime behavior across all consumers. Controls without a runtime consumer are removed; controls with narrower behavior are renamed to their true scope.

## Truthful Apple/AirPods Handling

### Problem

The current local detector can turn an AirPods association plus a coarse Apple activity value into `Possible Remote Listening`, threat level 2, threat counts, and high-priority notifications even though BLE cannot establish Live Listen, microphone routing, captured audio, ownership, proximity intent, or eavesdropping.

### Required Android behavior

Normalization is a pure Android function applied immediately after each local/backend/badge source maps one record and before source merge, severity sorting, threat counting, notification policy, or any Android Privacy detail presentation. The current History store contains sky detections rather than Privacy findings, so this overhaul does not add a Privacy-history schema or migration.

One record is Apple-related when that same record contains Apple manufacturer ID `0x004C`, an Android-decoded Apple continuity type, or an explicit normalized Apple vendor field. It is AirPods-related when that same record contains an AirPods model/service/decoded Nearby Info flag. A listening-oriented kind/title/detail on an Apple-related record is neutralized even when AirPods detail is partial. Android never correlates separate devices, sources, rotating IDs, or observations across a time window to manufacture this evidence. If only Apple evidence is known, use generic Apple activity wording; if neither Apple nor AirPods evidence is present, preserve the unrelated category.

- Local Apple/AirPods correlation produces an informational Apple continuity observation, never `REMOTE_LISTENING`.
- Android normalizes backend-fed and badge-fed rows only when Apple/AirPods evidence accompanies listening-oriented category or wording.
- Unrelated non-Apple listening categories are not silently rewritten.
- Title: `AirPods connection/activity nearby` when ownership is unknown, or the owned device name when known.
- Apple-only fallback title: `Apple device activity nearby`.
- Detail: `An Apple device reports connected AirPods and media, call, or video activity.`
- Apple-only fallback detail: `An Apple device reports a nearby activity state; the specific activity is unavailable.`
- Limitation: `Live Listen and microphone use cannot be determined from BLE.`
- The observation does not contribute to threat count or automatic category expansion.
- The observation does not trigger a high-risk privacy notification.
- Bonded/owned devices are labeled `Your device` and excluded from threat counts.

The old listening alert/category is removed from every Android-visible live source and from notification eligibility. No Android text may use `possible listening`, `remote listening`, `eavesdropping`, or comparable intent claims for this heuristic.

## Screen Designs

### Welcome and contextual permissions

Welcome is shown only until onboarding completion. Its primary action is above optional links/update information. It explains:

1. what AR/sky and privacy scanning can detect;
2. important limitations;
3. which data may remain local or be sent to configured services;
4. that permissions are requested when the relevant feature is first used.

Startup no longer launches a blanket permission batch over Welcome. Location, camera, Bluetooth/Wi-Fi, notifications, and microphone are requested contextually. Permanent denial states include `Open app settings`; approximate-only location and notification-channel state are represented accurately.

The contextual triggers are explicit: AR/IR requests camera when the user enters the camera feature, AR/Map requests location when location-dependent content is first used, Phone privacy scan requests the API-level-appropriate Nearby Devices/Bluetooth/Wi-Fi permissions when that collector is enabled, alerts request notification permission when alerts are enabled, and ultrasonic requests microphone when ultrasonic is enabled. Denying one permission does not block unrelated destinations.

Update checking moves to Info and compares ordered version/version-code values rather than treating any unequal string as an update.

### AR

AR retains its camera/compass purpose but simplifies its command layer:

- consolidate status overlays so labels do not collide with compass or bottom chrome;
- use responsive dp-based geometry and minimum touch targets;
- tapping an object opens a concise Object Peek;
- Object Peek offers explicit `Inspect`, `Capture`, and `Full details` actions;
- a normal label tap and opening Zoom never write photos;
- Capture opens one shared review flow with explicit `Save`, `Share`, and `Discard`;
- remove the categorical `not in any aircraft database` claim unless an actual lookup result supports it;
- camera initialization failure shows retry and permission recovery instead of log-only failure;
- every rendered target has an accessible semantic representation.

### Map

Map remains map-first:

- use one compact search/filter HUD rather than a full control stack over the map;
- show active-filter count and a clear reset action;
- move advanced filters into a sheet;
- add a visible legend and visible remote-search entry instead of a hidden long press;
- tapping a marker opens the same concise Object Peek used by AR/List;
- provide an accessible target list alternative;
- show location/tile/network failure states without presenting an unrelated default region as valid context.

Existing map and detection feeds remain in scope; no backend or map-provider contract changes are required.

### List

List shows aircraft and drone results immediately after its compact search/filter header. Remove the entire badge status/control/firmware/appearance/display-policy panel from this destination.

List distinguishes:

- loading;
- live results;
- no detections;
- no filter matches with `Clear filters`;
- offline/stale results with age;
- source failure.

Rows use responsive multiline content and shared category/source semantics. Selecting a row opens Object Peek, then Full details on request.

### Privacy

Privacy preserves the current merged findings list and tap/detail journey but contains no badge configuration and no unrelated sky-alert footer.

The top of the screen contains:

- title and last update;
- one compact source-health summary for Phone, Backend, and Badge;
- search;
- concise filter chips with active-filter count.

Findings are ordered by actionable severity and then recency. Each row shows the best available subset of:

- clear, uncertainty-aware title;
- severity marker with text, not color alone;
- source;
- last seen age;
- owned/bonded label;
- signal in dBm;
- concise evidence/limitation;
- detail affordance.

Rows expose Ignore, Track, or RSSI sweep only when the source-specific capability supports it. The direction feature is named `RSSI direction sweep`, not `device locator`, until its confidence model can justify a stronger claim. Cancel always ends an active scan and sensor collection has an explicit lifecycle.

New critical findings are clickable, announced through accessibility live-region semantics, and deep-link to the exact finding. Informational and owned devices remain visible without inflating the threat count.

An item-specific notification carries the destination plus the finding's source kind and source record ID. Android emits an item-specific critical notification only when it can construct that routable key. If the row expires before the tap, Privacy opens an `Item no longer current` state for that key instead of substituting another row.

Privacy's primary outcome is the findings list, not a dashboard or configuration form. On first load with no cache it shows source-level loading. Once sources resolve, it distinguishes `No current findings` from `No matches` and offers `Clear filters` only for the latter. One failed or paused source remains visible in the source-health summary without replacing valid findings from other sources. Cached rows display their stale age, stop generating new critical alerts, and leave Current at the source-specific expiry threshold. Each rendered row includes severity text, source, freshness, ownership, signal, and evidence/limitation when those values exist; missing values are omitted rather than replaced by invented defaults.

### Badge

Badge is the fifth triangle/tune top-level destination and the only top-level badge-configuration surface.

The default hierarchy is:

1. **Connection summary:** verified device/transport, live/stale/error state, refresh/reconnect action.
2. **LCD preview:** current Android-known badge presentation, clearly labeled when it is a local preview rather than verified device state.
3. **Appearance:** only confirmed, visibly supported palette/theme/brightness controls.
4. **Display rules:** readable category rows with plain-language lane/proximity behavior, consistent enable/disable semantics, and active/applied state.
5. **Apply area:** dirty-state summary, `Revert draft`, and `Apply changes`; success/failure/verification shown adjacent to the action.
6. **Advanced device tools:** diagnostics and device recovery as secondary routes.

Appearance and display policy use one consistent draft/apply/revert interaction. Reset does not bypass the draft model or send immediately. Switching sections does not discard edits.

Theme swatches and selection controls have text labels, selected semantics, and 48dp targets. A class cannot be simultaneously enabled with lane `OFF`; disabling a class disables or hides its dependent editors.

Reboot and bootloader operations are never shown as ordinary peer actions. They live in a danger/recovery surface with:

- transport capability check;
- known device/target check;
- explicit confirmation;
- semantic progress;
- disabled conflicting commands;
- success/failure and reconnect/verification guidance.

The current arbitrary-file in-app firmware picker/upload action is removed from this release. Advanced Device Recovery may show Android-read firmware version/status and explain that updates happen outside this app, but it does not link, select, upload, or flash a firmware image. Reintroducing in-app firmware upload requires a separately approved signed/targeted package design.

If Android cannot validate enough information to offer reboot or bootloader safely, the operation remains unavailable and explains the required supported USB connection.

### History

History routes by immutable history row ID. Detail loads the exact selected snapshot and labels it `Historical detection`; it never silently prefers a live object or the newest row for the same object ID.

History adds:

- visible screen title;
- active-filter summary;
- loading, empty database, no matches, and error states;
- multiline responsive rows;
- accurate source/category labels;
- local data-management actions for clear/delete and a short retention/location explanation.

History export is out of scope for this overhaul. Clear/delete and the retention/location explanation are required. Row delete and Clear all require confirmation. If the existing Android store has no automatic retention policy, the explanation says records remain local until the user clears them rather than implying an expiry that does not exist.

### Info

Info remains the seventh top-level destination and keeps the bottom bar visible. It is organized in this order:

1. source/permission status;
2. Settings;
3. Guide and category legend;
4. Privacy & Data;
5. About, support, version, and updates;
6. Advanced.

Settings rows are whole-row semantic toggles with accurate effective OS state. `Sensor Backend Connection` is retained and gates every Android backend poller in AR, Map, and Privacy; disabling it clears remote Current/cache state without deleting persisted History. `Privacy Scanner` is renamed `Phone privacy scan` because it controls local collectors only. Ultrasonic enablement requests/checks microphone permission.

Privacy & Data accurately states that:

- History may store observations and phone coordinates locally;
- ADS-B/weather requests may send location to third-party services;
- a configured sensor backend exchanges detection data;
- Calibration sends operator/session GPS to the configured backend when used.

Invalid backend URLs are rejected before save. Connection tests identify the actual endpoint and show neutral progress, success, or failure.

`Advanced > Triangulation Calibration` retains the existing feature but is de-emphasized. Define `calibrationEntryAvailable` as: the backend setting is enabled and a `/health` request to the configured backend has succeeded during the current app session. When false, the entry shows `Unavailable` and cannot open the calibration route. Refreshing the entry repeats that health request. When true, the entry may open the unchanged calibration flow, whose existing preflight remains responsible for token and calibration-endpoint readiness; the entry does not claim that calibration itself is ready. This overhaul only relocates the entry and adds this gate; it does not clean up calibration UI, lifecycle, submission, queueing, or algorithms.

### Detail and Reference Guide

Full Detail becomes summary-first:

- identity/category/source/freshness and important position facts first;
- explicit live versus historical label;
- partial-data and retry state;
- copyable identifiers;
- advanced/raw sections collapsed by default;
- no fixed 40/60 rows that break large text.

Aircraft and Drone reference catalogs share one scaffold and one route with encoded arguments. Search has a zero-result state, tab/search state is saveable, informational tags are not fake filter controls, and sensitive category claims include provenance or appropriately cautious wording.

### EMF and IR tools

Rename `EMF Sweep` to `Magnetic-field sweep`. It reports magnetometer readings and baseline deviation without claiming to identify hidden electronics. Initial/unavailable state does not show `0 µT / NORMAL`; it explains sensor availability, accuracy, and baseline/reset.

IR uses `possible IR-like light` rather than claiming a camera. Preview analysis coordinates are transformed for crop and front-camera mirroring. Camera permission is explained before request and permanent denial/camera bind failure includes recovery.

## Visual System

The visual direction is a density and hierarchy brief, not a pixel-perfect dependency on a mockup. Implementation uses the existing Material 3 theme and code-native Compose components:

- compact app bars and filters;
- calm neutral/lavender-gray surfaces;
- blue/teal interaction emphasis;
- amber for attention and red only for destructive/error state;
- restrained cards with one information level per surface;
- multiline content instead of ellipsizing primary information;
- no glassmorphism, neon/cyberpunk decoration, oversized metric cards, or nested scroll regions;
- shared components for source health, finding rows, empty/error states, Object Peek, section headers, and confirmation/progress flows.

The certified route matrix supports 1.0x and 1.3x font scale without overlap, unreachable actions, or wrapped navigation labels. It includes all seven top-level routes plus Object Peek, Full Detail, Capture Review, Map advanced filters, History detail, Reference Guide, `Privacy > Ignored devices`, Badge Diagnostics, Badge Device Recovery, the Calibration unavailable entry, EMF, IR, permission-recovery states, and long confirmation dialogs. The seven-destination shell is additionally checked at 2.0x with icon-only navigation permitted. TalkBack descriptions do not duplicate visible text, and color is never the sole carrier of category/severity/selection.

## Error and Recovery Rules

- Keep the last good data only when it is labeled cached/stale with age.
- Never turn a fetch/transport failure into an empty-success state.
- Show one concise inline recovery action close to the failure; use snackbar/live region for transient command results.
- Do not expose raw exceptions, UUIDs, stack details, or HTTP bodies in normal UI.
- A retry cannot issue duplicate destructive commands.
- Leaving a pending badge mutation triggers the appropriate confirmation/cancel path.
- Notification taps include route and item identity; missing/expired items open a truthful unavailable state rather than an unrelated screen.

## Implementation Stages

### Stage 0: Regression tests and trust fixes

- Lock in neutral Apple/AirPods behavior across local, backend-fed, and badge-fed Android presentation.
- Fix exact History snapshot routing.
- Remove AR/Zoom auto-save paths and false database claim.
- Make settings observable and wire/remove placebo controls.
- Correct privacy/data and sensor terminology.
- Add confirmation and capability gating for device-mutating actions.

### Stage 1: Shell and route ownership

- Add the stable main graph and first-run Welcome gate.
- Implement the compact seven-button bar.
- Add Badge as top-level; remove Cal from top-level; move Calibration to Info Advanced.
- Keep Info and Badge bottom navigation visible.
- Remove badge controls from List and Privacy.

### Stage 2: Core destination redesign

- Build Badge configuration and app-scoped connection presentation.
- Reformat Privacy as the clean findings list.
- Refactor List, History, and Info around shared state/empty/error components.

### Stage 3: AR, Map, and secondary screens

- Introduce Object Peek and explicit capture review.
- Compact the Map HUD and filters.
- Make Detail, Reference, EMF, and IR secondary flows responsive and truthful; relocate Calibration and add only its unavailable entry state.

### Stage 4: Accessibility, regression, and release verification

- Complete Compose interaction/semantics tests.
- Test normal and large-font layouts.
- Run JVM tests and assemble a debug APK.
- Install on `Pixel8_API35` and walk every route in the certified matrix, every permission state and error state available in isolation, and all seven navigation buttons.

## Testing Strategy

### JVM unit tests

- Apple/AirPods activity cannot produce listening/threat presentation or notification eligibility.
- Non-Apple listening categories remain unchanged.
- owned devices do not contribute to Privacy threat count.
- source freshness produces live/stale/expired presentation and stale data cannot generate a new alert.
- source-aware Ignore/Track/RSSI-sweep capabilities are enforced.
- source-aware Ignore persistence, reversal, and duplicate isolation are enforced.
- exact history row ID loads the selected snapshot.
- AR label tap, Object Peek, Zoom, Share, and Discard write no saved photo through an injected fake writer; only `Capture > Save` writes once, and the unsupported database claim is absent.
- filter active count, reset, unknown-distance policy, and no-match behavior.
- backend setting gates all Android pollers and clears remote state.
- badge transport/capability and draft/apply/ack/failure state.
- unsupported BLE display-policy remains unavailable and in-app firmware upload is absent.
- `calibrationEntryAvailable` follows the backend-setting plus current-session `/health` predicate.
- app version ordering and invalid backend URL validation.

### Compose/UI tests

- onboarding completion and stable seven-destination navigation/back-stack behavior;
- all seven top-level buttons remain reachable and route to the correct destination;
- List and Privacy contain no badge configuration controls;
- Badge contains appearance/display configuration and retains drafts across section changes;
- History opens exact snapshot semantics;
- Privacy orders actionable severity before recency and renders only the available row fields/actions;
- notification/deep-link routing opens the exact keyed finding or its explicit expired state;
- contextual permission prompts do not appear on startup or block unrelated routes;
- History row delete/Clear all confirmation and local-retention copy;
- the unavailable Calibration entry cannot navigate, while an available entry opens the unchanged flow;
- permission denied/permanently denied recovery;
- loading, empty, no matches, stale, failed, and unsupported states;
- confirmation/progress around reboot and bootloader, plus absence of in-app firmware upload;
- 1.0x and 1.3x font scale on the certified route matrix, plus 2.0x shell/navigation behavior;
- minimum touch targets and selected/expanded/live-region semantics.

### Verification commands

From `android/`:

```bash
./gradlew testDebugUnitTest
./gradlew assembleDebug
./gradlew connectedDebugAndroidTest
```

Start `Pixel8_API35` before `connectedDebugAndroidTest`. Use the Android emulator QA workflow with two API 35 configurations: a 412dp-wide `Pixel8_API35` profile and a 360dp-wide compact profile. Traverse the complete certified route matrix at 1.0x and 1.3x font scale on both portrait widths, check the seven-destination shell at 2.0x, and rotate both profiles to cover bottom-bar behavior below 600dp and navigation-rail behavior at 600dp or wider. Capture screenshots/UI trees for the core screens, exercise onboarding and route restoration, and inspect logcat for crashes or repeated errors.

Before any physical transport is labeled verified for release, use the user-provided Android phone and badge for an Android-only smoke test. Do not update or flash firmware. For USB, Badge AP, and BLE paths intended to be enabled, read valid status across at least two refresh intervals, exercise one reversible command from each enabled command class, verify the returned state, and restore the original badge configuration. Test reboot/bootloader only with separate explicit user approval; otherwise those controls remain unavailable. Record unavailable transports as unverified rather than converting them to a pass.

## Acceptance Criteria

The overhaul is complete only when all of the following are true:

- The app has exactly seven top-level destinations in the approved order: AR, Map, List, Privacy, Badge, History, Info.
- The bottom bar works on all seven top-level destinations without clipped/wrapped selected labels at tested font scales.
- Onboarding completion and the last valid top-level route restore after process recreation; an invalid saved route falls back to AR, and Back from a top-level route exits instead of replaying tabs.
- Privacy is a current-findings list and contains no badge configuration; it visibly distinguishes source health, loading, no findings, no matches, stale data, partial-source failure, and full failure.
- Every Privacy row uses only available evidence, exposes source/severity/freshness/ownership/signal/limitation data when known, renders only supported source-specific actions, and sorts by actionable severity then recency.
- Ignored devices persist by source and stable ID, can be restored from `Privacy > Ignored devices`, and do not suppress unrelated or unstable identities.
- List contains no badge configuration.
- Badge is the sole top-level badge configuration surface.
- Badge controls match the conservative transport matrix; a mutation can report `Applied` or `Verified` only from the specified acknowledgement/post-state evidence, and a timeout never becomes success or an automatic retry.
- Android contains no arbitrary firmware-file picker, firmware upload, or firmware flash action.
- Calibration is reachable from Info Advanced and is not a top-level button; while `calibrationEntryAvailable` is false, it is visibly unavailable and cannot open the calibration route.
- Apple/AirPods activity is informational and cannot trigger the old listening claim, threat count, or high-risk notification on any Android live source.
- History loads the exact selected snapshot, labels it historical, supports local clear/delete, and explains local retention/location behavior without offering export.
- AR/Zoom never save without an explicit user action.
- Startup does not request a blanket permission batch; AR/camera, location-dependent views, phone privacy scan, alerts, and ultrasonic features request their API-appropriate permissions only at first relevant use and provide permanent-denial recovery.
- Critical-finding notifications and in-app announcements open the exact destination/item; an expired item opens a truthful unavailable state, never Welcome or an unrelated record.
- Settings and privacy/data copy match actual Android behavior; the backend toggle gates AR, Map, and Privacy remote pollers without deleting History, invalid URLs cannot be saved, and Info displays semantic app version plus the configured backend endpoint. Update availability is shown only when ordered version/version-code comparison finds a genuinely newer version.
- unsupported or unverified badge operations cannot be invoked as if supported.
- audited high-risk mutations have confirmation and visible progress/result state.
- normal, empty, stale, failed, denied, and unsupported states are distinguishable on core screens.
- every route in the certified matrix passes the agreed 1.0x/1.3x layout checks, and the seven-destination shell remains operable at 2.0x.
- both certified portrait widths and the deterministic 600dp landscape rail breakpoint pass navigation/layout checks.
- every transport labeled verified passes the Android phone-plus-badge smoke test; unavailable or untested transports remain visibly unverified with mutating controls disabled.
- required JVM and connected Compose/UI tests pass.
- a fresh debug APK assembles, installs, launches, and completes the emulator route walkthrough.
- no firmware or backend files are changed by the implementation.
