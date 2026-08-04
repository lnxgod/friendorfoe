# Android Aircraft, Update, and Backend Opt-In Restoration Design

**Date:** 2026-08-02

**Status:** Approved on 2026-08-02

**Affected surface:** `android/` only

**Implementation baseline:** The current `origin/main` Android application, including the v0.67.x interface overhaul and About-first landing. The checked-out local `main` is an older divergent snapshot and must not be used as the implementation baseline.

## Goal

Restore three Android product guarantees without rolling back the current interface:

1. Aircraft detail screens show the airplane imagery bundled with the app.
2. The main About landing checks the official GitHub release feed and clearly reports update status.
3. The configured Friend or Foe sensor backend makes no network request until the user explicitly enables it.

GitHub update checks, public ADS-B providers, weather, map tiles, and other non-Friend-or-Foe internet services are separate from the sensor-backend opt-in.

## Root Causes

### Aircraft imagery

Commit `98fb8ee` (`Android: finish human-centered interface overhaul (#6)`) routed full aircraft detail through `DetailOverviewContent`. The bundled assets, `AircraftPhotos` resolver, and legacy photo card remained present, but the new `DetailPresentation` model and renderer carried no aircraft visual. AR and Map sheets and the Reference Guide still have image-bearing code; the full detail route lost it at the call boundary.

### About update status

Commit `051c2c8` (`android: restore About-first landing`) created a stateless `AboutLandingScreen` and moved the existing version/update row into the secondary App settings route. The validated GitHub repository, version comparison, and failure handling remain functional, but the normal landing route no longer owns update state or renders it.

### Backend default

The v0.67.x application has observable backend settings and poll gates for AR, Map, and Privacy, but both `DetectionSettings.defaults()` and the missing-key `SharedPreferences` fallback still enable the configured sensor backend. The backend Retrofit client also lacks a final fail-closed opt-in check.

## Selected Approach

Use a targeted restoration with defense in depth:

- carry aircraft visual data through the new detail presentation model and reuse the existing asset resolver and silhouette fallback;
- reuse the current GitHub update repository and state model on the main About landing;
- change the backend preference default to off, retain lifecycle-aware poll gates, and add a network-client guard that prevents accidental or future callers from reaching the configured backend while disabled.

This preserves the current navigation, summary-first detail layout, update validation, and source-isolation work. It does not revert the interface overhaul or introduce new image assets, background workers, or notification channels.

## Aircraft Image Restoration

### Presentation model

`DetailPresentation` gains an optional aircraft visual descriptor containing the information the renderer needs:

- preferred photo URL, when the observation or saved history row already has one;
- aircraft type code, when known;
- aircraft description, when known;
- object category for the correct fallback silhouette and tint.

`presentLiveDetail` always supplies this descriptor for aircraft. It uses the enriched type and description when available, otherwise the live aircraft values. Drone presentations do not supply it.

`presentHistoricalDetail` supplies a descriptor only for aircraft history rows. It carries the saved `photoUrl` and saved category. Because the current history schema does not persist aircraft type, the implementation must not guess a bundled type photo for historical rows. A saved photo may render; otherwise the category silhouette renders.

### Rendering

`DetailOverviewContent` renders the aircraft visual between the title/status area and “At a glance.” The card preserves the previous resolution order:

1. observation or history `photoUrl`;
2. `getAircraftPhotoUrl(typeCode)`, which resolves a bundled `file:///android_asset/aircraft/...` image for live aircraft;
3. the existing category/type silhouette while loading, when no photo exists, or when image loading fails.

The card retains a concise aircraft content description. It does not fetch new image catalogs, add assets, or change the Reference Guide, AR sheet, or Map sheet behavior.

## About Landing Update Discovery

### State ownership

The top-level About route obtains the existing `AboutViewModel` and observes its `UpdateUiState`. About and its App settings child use the same navigation-graph-scoped ViewModel so an update result remains consistent when moving between the landing and settings.

The ViewModel adds an idempotent “check if idle” entry point. The About landing invokes it once for that ViewModel lifecycle. Ordinary recomposition or returning from App settings does not start duplicate requests. A new application process may check again.

### User experience

The landing shows the installed version and a reusable update row with the existing states:

- **Checking for updates** while the official feed is loading;
- **Up to date** when the installed semantic version is equal to or newer than the latest valid release;
- **Update available** with the remote version and an **Open** action;
- **Could not check for updates** with a **Retry** action after network, HTTP, or metadata failure.

The Open action uses the release URL already validated by `AppUpdateRepository`. Failure never blocks About, App settings, or local app features. This remains an in-app status row; no Android system notification or background worker is added.

### Network boundary

The check continues to use GitHub’s official `lnxgod/friendorfoe` latest-release endpoint through the existing `AppUpdateApi` and `AppUpdateRepository`. It is not routed through the configured Friend or Foe backend client and is not controlled by the sensor-backend preference.

## Explicit Sensor-Backend Opt-In

### Preference and upgrade behavior

Both canonical defaults become `sensorBackendEnabled = false`:

- `DetectionSettings.defaults()`;
- `DetectionPrefs` when `sensor_backend_enabled` is absent.

This produces the intended upgrade behavior:

- fresh install or previously untouched implicit default: off;
- explicitly stored `false`: remains off;
- explicitly stored `true`: remains on as the available evidence of prior opt-in, including an Android backup restore.

No migration writes `true` for a missing key.

### Polling and request gates

AR, Map, and Privacy retain their reactive polling gates. When backend use is off, those collectors perform no fetch and expose paused/off state rather than repeatedly failing. Disabling the setting cancels the active polling generation and clears only backend-derived live state; local observations and History remain intact.

The shared `backendClient` adds a final request interceptor that reads the preference at request time. If the backend is disabled, it throws a local, typed `IOException` before URL resolution and does not call `chain.proceed`. Invalid backend URLs also fail closed rather than falling through to Retrofit’s placeholder localhost URL. This guard applies to `SensorMapApiService`, calibration, dormant callers, and future callers that might otherwise bypass a screen-level gate.

The URL-rewrite interceptor continues to use the currently configured valid endpoint when the preference is enabled.

### Explicit actions and backend-only mode

Connection testing and Calibration backend actions remain unavailable while the sensor backend is off. Opening screens, restoring navigation state, or resuming the app must not implicitly enable it.

Backend-only mode may be enabled only while the sensor backend is enabled. Turning the sensor backend off also normalizes backend-only mode to off. A legacy state containing `sensorBackendEnabled = false` and `backendOnlyMode = true` is projected and persisted as backend-only off, preventing both local and remote detection from being unintentionally disabled.

## Error and Lifecycle Behavior

- Missing or failed aircraft images show the existing silhouette; they do not remove the detail content.
- GitHub failure shows neutral retry copy and preserves the installed version display.
- Backend-disabled state is intentional and is presented as Off or Paused, not as a connection failure.
- Enabling the backend starts eligible visible pollers through the existing reactive settings flow.
- Disabling it cancels in-flight polling generations; late results from the old generation cannot repopulate remote state.
- The network guard is evaluated for every backend request, so disabling takes effect without recreating Retrofit or restarting the app.
- No sensor-backend request is triggered merely by ordinary launch onto About.

## Testing Strategy

### JVM unit tests

- `DetailPresentationTest`: live aircraft carries the expected photo/type/category descriptor; historical aircraft uses only saved photo/category evidence; drones carry no aircraft visual.
- `AircraftPhotosTest`: representative bundled type resolution, normalization, known variant mapping, null, and unknown behavior.
- `AboutViewModelTest`: idle auto-check starts once, a genuinely newer release becomes Available, equal/older remains Up to date, failure becomes retryable, and repeated “check if idle” calls do not duplicate work.
- `DetectionSettingsTest`: product defaults have the sensor backend off.
- `DetectionPrefsTest`: a missing key is off while explicit true and false values survive recreation.
- `BackendPollingGateTest` plus AR/Map/Privacy backend tests: initial off performs zero fetches, explicit enable starts, disable cancels and clears, and late prior-generation results are ignored.
- A focused backend-client interceptor test: disabled and invalid configurations never call the downstream chain; enabled configuration rewrites to and proceeds against the exact configured endpoint.
- About/settings tests cover backend-only normalization when backend use is disabled.

### Compose tests

- `DetailOverviewContentTest`: an aircraft descriptor renders the tagged image card; a drone presentation does not.
- `AboutLandingScreenTest`: Checking, Up to date, Update available/Open, and failure/Retry states are visible and invoke the correct callbacks.
- Navigation coverage proves the production top-level About route owns the updater and App settings observes the same update state.
- Settings coverage proves the backend switch is off on fresh state and backend-only cannot remain active while it is off.

### Build and emulator verification

From `android/`:

```bash
./gradlew testDebugUnitTest
./gradlew assembleDebug
./gradlew connectedDebugAndroidTest
```

On an API 35 emulator:

1. Clear app data and launch normally; About opens, GitHub status resolves, and the sensor-backend switch is off.
2. Visit AR, Map, and Privacy while backend use is off; confirm no requests or repeated configured-backend errors in logcat.
3. Open a known live aircraft full-detail route and confirm a bundled photo; exercise a missing image and confirm the silhouette.
4. Enable the backend explicitly and confirm eligible polling can begin; disable it and confirm remote state clears and requests stop.

Connected tests may be limited to the available emulator, but unit tests and `assembleDebug` are required before completion.

## Scope Boundaries

- Android source, Android tests, and this Android design/plan documentation only.
- No backend service, database, API, deployment, or configuration changes.
- No ESP32 firmware, build, upload, or hardware changes.
- No new aircraft or drone image assets and no image generation.
- No release publication, version bump, tag, APK upload, or GitHub release mutation unless separately requested.
- No changes to public ADS-B, weather, map-tile, HexDB, badge-local-AP, debug-bridge, or GitHub update availability beyond the update row described here.

## Acceptance Criteria

- Full live aircraft detail again displays a real bundled or observation-provided airplane image when available and a silhouette otherwise.
- Historical aircraft detail displays its saved photo when available and never invents a type photo unsupported by the stored snapshot.
- The normal About landing automatically checks the existing validated GitHub Releases feed once per ViewModel lifecycle and renders all update states with Retry/Open actions.
- The sensor-backend preference is off when no explicit value exists.
- An explicitly stored enabled value remains enabled; explicit false remains disabled.
- While disabled, AR, Map, Privacy, connection testing, Calibration, dormant consumers, and future callers produce zero configured-backend network attempts.
- Enabling the setting is the only action that permits configured-backend traffic, and disabling it takes effect immediately.
- Backend-only mode cannot disable local collection while the backend itself is off.
- GitHub updates and public ADS-B feeds remain independent of the sensor-backend opt-in.
- Focused tests, the Android JVM suite, debug assembly, and the available emulator verification pass.
- No backend or firmware files are changed.

## Self-Review

- The design contains no placeholders or deferred decisions.
- “Backend” is explicitly limited to the configured Friend or Foe sensor backend.
- The GitHub update check and sensor-backend opt-in do not contradict each other.
- The aircraft restoration uses existing evidence and assets without changing unrelated visual surfaces.
- Upgrade behavior distinguishes an absent implicit default from an explicitly stored opt-in.
- Polling gates and the request interceptor provide complementary, testable guarantees.
