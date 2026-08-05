# Android About, Badge Mark, Map, and Alert Cleanup Design

## Objective

Restore the Android app's human-friendly About landing and Badge Triforce identity, make the map open directly at the phone's location without a long camera animation, and remove speculative "Possible Serial Skimmer" findings and alerts. This change is Android-only: no firmware or backend production behavior changes.

## Confirmed Product Behavior

- Every ordinary app launch opens the About destination.
- A privacy-notification launch still deep-links to the exact current finding.
- The app keeps seven top-level navigation destinations.
- The seventh destination is labeled **About** rather than **Info**.
- The Badge destination uses the gold three-triangle Triforce mark in both the bottom navigation bar and landscape navigation rail.
- The Badge screen repeats that mark in its top status/header treatment.
- The current working settings and advanced tools remain available, but they are not the landing experience.
- No Android screen or notification shows a speculative possible-skimmer finding.

## About Landing and Settings Navigation

The existing top-level `info` route remains stable for saved state and deep-link compatibility, but its user-facing name becomes About. Its default content is a dedicated About landing modeled on the prior welcome/landing presentation rather than the current settings-first list.

The landing contains:

1. The Friend or Foe name and Triforce mark.
2. A concise description of aircraft, broadcast-drone, and supported privacy observations.
3. A prominent DEF CON message: "Were you at our DEF CON talk? Thank you for coming—we're glad you're here."
4. Clear evidence language explaining that observations are not proof of identity, intent, or ownership.
5. Version, support, GitHub, and reference-guide actions.
6. One primary **App settings** action.

**App settings** opens a secondary `info/settings` destination containing the existing `InfoContent` configuration/status list. The secondary screen uses the standard back header and does not add an eighth top-level destination. Calibration and advanced tools stay inside the existing settings/advanced content.

Fresh onboarding remains available for installs that have not completed it. Completing onboarding enters the About landing. Normal launches ignore the previously persisted last top-level route and start at About. In-session top-level state restoration continues to work. Privacy intent routing runs after the navigation graph is ready and therefore overrides the normal About start only for a valid finding route.

## Triforce Badge Identity

The existing firmware-independent `BadgeMarkIcon` vector is the single source of truth. `TopLevelDestination.BADGE` uses it instead of the generic Tune icon. Navigation renders the mark using normal selected/unselected Material colors, with the existing gold accent available for Badge-specific header treatment.

The Badge screen adds a compact header row containing the gold Triforce and "Badge" title before USB status. It does not simulate badge hardware or change any badge command, theme, display-policy, USB, BLE, or firmware contract.

## Map Camera Policy

The current bug has two causes: osmdroid creates the map at its default `(0, 0)` center, and the first valid phone position is applied with `animateTo`. That produces the visible cross-country/world pan. Later location updates can also call `animateTo` whenever the camera is more than 500 meters away.

The corrected camera policy is explicit and testable:

- Before a valid usable phone position exists, show a lightweight locating surface instead of exposing the default `(0, 0)` map camera.
- On the first valid position, set a city-level zoom and center synchronously with `setCenter`; never animate.
- After initialization, ordinary GPS updates move the user marker but do not move the camera.
- When compass-follow mode is enabled, camera recentering uses `setCenter`, not `animateTo`.
- Any map touch marks the camera as user-controlled and prevents automatic recentering until the user explicitly enables follow again.
- If location permission is unavailable, preserve the current browse-without-location experience and permission recovery panel; do not wait forever for a GPS fix.

A small pure camera-policy reducer determines whether a render should wait, initialize instantly, follow instantly, or leave the camera unchanged. The osmdroid controller only executes the reducer's result.

## Possible-Skimmer Removal

The user-visible warning originates in the Android `BleThreatAnalyzer` serial-service heuristic. It emits `BleThreatSignal.SerialSkimmer`, which `GlassesDetector` converts to a high-severity `GlassesDetection` titled "Possible Serial Skimmer". That finding is eligible for both the Privacy list and notifications.

The Android speculative serial-skimmer path is removed rather than renamed:

- Stop producing the serial-skimmer behavioral signal and detection.
- Remove the serial-skimmer branch from behavioral detection arbitration.
- Keep unrelated pairing-spam behavioral analysis intact.
- Defensively reject legacy/residual Android findings whose match reason is `ble_behavioral:serial_skimmer` before Privacy presentation or notification publication.
- Defensively suppress backend rows explicitly categorized as `SKIMMER` in the Android adapter so the app cannot reintroduce the fear copy from stale or older service data.
- Leave badge firmware display classes and backend implementation unchanged.

This is a removal of an unsupported speculative claim, not a relabeling to another threat name.

## Error and Edge Handling

- Invalid persisted top-level routes sanitize to About.
- Valid privacy notification routes continue to take precedence over the normal launch destination.
- A missing location permission shows the existing recovery UI immediately.
- A valid approximate location may initialize the map, while precise distance/bearing overlays remain hidden under the existing permission policy.
- A temporarily missing GPS fix shows locating state without revealing `(0, 0)`.
- The About landing remains usable when update checks, support intents, or reference navigation are unavailable; unavailable actions are disabled or fail through the current safe URI handling.

## Test Strategy

Test-first regression coverage will prove:

1. The normal launch route is About, invalid saved routes fall back to About, and valid privacy notification routing still overrides it.
2. The seventh destination label is About and the Badge destination uses `BadgeMarkIcon`.
3. The About landing shows the Friend or Foe identity, DEF CON thank-you, and App settings action; that action opens the existing settings content.
4. Map camera policy waits for a usable position, initializes with an instant center, never emits an animation, leaves ordinary GPS updates alone, and stops following after user touch.
5. Phone behavioral analysis no longer creates possible-skimmer detections; pairing-spam behavior remains intact.
6. Legacy phone and backend skimmer records do not appear in current Privacy findings and are not alert-eligible.

Verification consists of the focused JVM tests, relevant Compose instrumentation tests, a clean `testDebugUnitTest assembleDebug` build, and emulator QA covering normal launch, settings navigation/back, Badge Triforce rendering, a mocked Las Vegas first location with no visible camera travel, user map pan, and Privacy absence of possible-skimmer copy.

## Scope Boundaries

- Android application source, tests, and Android release metadata only if a release is later requested.
- No ESP32 source, build, display-policy, or firmware changes.
- No backend production source or API behavior changes.
- No new navigation destination count, new detection feature, badge simulator, or speculative replacement alert.
