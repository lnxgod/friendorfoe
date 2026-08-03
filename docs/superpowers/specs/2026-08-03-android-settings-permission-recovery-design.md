# Android Settings and Permission Recovery Design

**Date:** 2026-08-03

**Status:** Design approved and self-reviewed on 2026-08-03; user written-spec review pending

**Affected surface:** `android/`, Android release workflow metadata, Android tests, and release documentation

**Implementation baseline:** Branch `android/regression-restoration` at the signed `v0.67.8-android-regression-restoration-rc1` release.

## Goal

Repair the Android Settings regressions and make runtime permission handling complete, contextual, and durable:

1. Every visible setting control responds to a tap and truthfully distinguishes configured state from an inactive prerequisite.
2. Android requests the permissions a feature needs only when the user enables or opens that feature.
3. A granted permission reliably completes the action that requested it, including Ultrasonic sampling.
4. Denial, permanent denial, revocation, navigation, and activity recreation always leave an actionable recovery path.
5. The backend switch controls only polling of the configured Friend or Foe sensor backend. Public ADS-B, weather, GitHub updates, map tiles, and other public online services remain independent.
6. The fix ships as a signed, tagged GitHub release that the About page can discover and open for installation.

## Evidence and Root Causes

### Manifest coverage

The manifest already declares every permission family used by production Android code:

- camera;
- fine and approximate foreground location;
- legacy Bluetooth permissions through Android 11;
- Bluetooth scan, connect, and advertise permissions on Android 12 and newer;
- nearby Wi-Fi devices on Android 13 and newer;
- Wi-Fi state access;
- microphone recording;
- notifications on Android 13 and newer;
- legacy external-storage write access through Android 9;
- internet and network state.

USB badge access correctly uses Android's per-device `UsbManager.requestPermission` contract rather than a manifest runtime permission. The app has no background location collector, foreground location service, or scheduled background scanner, so it does not need background-location or foreground-service permissions.

No new manifest permission is required. The defects are request construction, completion ownership, recovery UI, and permission-sensitive call boundaries.

### Android 12+ location request construction

`FeaturePermissions` currently requires fine location without approximate location for local radio/privacy discovery. Calibration constructs the same malformed request separately. Android 12 and newer require applications to request fine and approximate location together; a fine-only request can be ignored. The same defect reappears when approximate location is already granted and the app tries to upgrade by requesting fine alone.

This can leave BLE Remote ID, Wi-Fi Remote ID, Phone privacy scan, Wi-Fi anomaly detection, follower detection, and Calibration unavailable even though the user attempted to grant access.

### Permission completion ownership

`rememberPermissionBindings` currently discards the authoritative `ActivityResult` grant map. It signals that runtime permissions changed, then launches a second platform refresh in a composition-owned coroutine. The durable pending feature and the setting-enablement callback therefore depend on UI lifetime and a potentially stale second read.

Fresh-install emulator evidence shows this failure: Android granted `RECORD_AUDIO`, protected sources restarted, and the process remained alive, but Ultrasonic sampling stayed off. `AboutViewModel.resolvePendingPermission` also clears its pending setting for every result, including `Loading` or a stale non-usable result, so a later correct lifecycle refresh has no intent left to complete.

### Silent setting rows

Follower detection and Wi-Fi anomaly are rendered with `toggleable(enabled = false)` when Phone privacy scan, its permissions, or phone collectors are unavailable. Backend-only mode is similarly disabled when configured-backend polling is off. These rows look like switches, but Android intentionally drops their taps. The current rendering also forces `checked && enabled`, which hides a stored configured-on preference as an unchecked control.

### Incomplete recovery surfaces

- List starts location work without binding a visible permission request or recovery action; failures are caught and silently stop distance/location behavior.
- Calibration can request fine location without approximate location, and after a denial its disabled Start control cannot re-open the request or system settings.
- Legacy Android photo saving can reach a permanent storage denial without a system-settings recovery action.
- Opening notification-channel settings or app details silently ignores launch failure and has no fallback.
- Privacy source enablement also completes through a composition callback and can lose the action across navigation or recreation.

### Permission-sensitive runtime boundaries

Android lint reports nine `MissingPermission` errors. Five are guarded or caught across an abstraction boundary, but three expose real check/use or asynchronous revocation races:

- BLE privacy scan callback processing can read protected device data after permission revocation.
- sky-alert notification posting checks permission before `notify()` but does not contain a revocation between those operations.
- Wi-Fi Aware `subscribe()` runs later in a framework callback, outside the repository's collector exception boundary.

`BleFeatureExtractor` also uses Bluetooth transport type as if it were public/random address type. It has no production callers, but the contract is incorrect and causes a permission-sensitive device lookup.

## Selected Approach

Use a centralized, feature-contextual permission coordinator plus truthful, always-actionable Settings rows.

This is preferred over:

- a minimal request-set patch, which would leave lost completion, silent rows, and dead-end denial paths;
- a first-launch permission wizard, which would ask for unrelated sensitive access before the user chooses a feature and would still require contextual recovery later.

The selected approach keeps permission prompts tied to user intent, uses pure request-planning logic that can be tested across SDK versions, makes result completion ViewModel-owned, and hardens the actual protected API boundaries.

## Permission Architecture

### Canonical feature requirements

`requiredPermissions(feature, sdk)` remains the single source for feature requirements. Local radio/privacy discovery must include both fine and approximate location on Android 12 and newer, in addition to the SDK-specific Bluetooth and nearby Wi-Fi permissions already required.

Calibration must consume the same canonical request-planning helper rather than assemble its own location/Bluetooth array. Its feature-specific requirement remains precise foreground location plus Bluetooth advertise/scan/connect where the SDK requires them.

### Request plan normalization

A pure request-plan function accepts:

- the feature;
- SDK level;
- currently granted permissions.

It returns the exact array to launch. Whenever fine location is in the launch set on Android 12 or newer, approximate location is included in the same launch even if approximate location is already granted. This preserves Android's required paired-request contract for both first request and precise-location upgrade.

Already-granted unrelated permissions are not requested again. No screen manually appends location or Bluetooth strings outside this helper.

### ViewModel-owned result completion

The Activity Result callback passes its returned grant map to `PermissionStateViewModel`. That ViewModel owns completion in `viewModelScope` and keeps the pending feature in `SavedStateHandle` until resolution is final.

Completion follows this order:

1. Record the actual result map for the pending feature.
2. Notify protected-source owners that runtime permissions changed.
3. Refresh and publish the canonical feature state from the permission repository.
4. Clear the pending feature only after a usable grant or a confirmed denial/permanent denial has been published.

`Loading` never clears pending intent. UI disposal, navigation, ordinary recomposition, and activity recreation cannot cancel the completion. Composition callbacks may observe state for presentation, but they are not responsible for durable completion.

`AboutViewModel` keeps its pending Settings action in `SavedStateHandle`. When the matching feature becomes usable, it writes the requested setting exactly once and clears the pending action. If the result is denied, the pending action becomes a recoverable denied state rather than disappearing on a transient refresh. Privacy source enablement uses the same durable state-driven pattern instead of a composition-owned completion lambda.

### Contextual timing

The app does not request all permissions at launch. A system permission dialog may be initiated only after the user:

- enables a permission-backed setting;
- opens a feature whose primary operation requires the permission and continues from its rationale;
- taps an explicit recovery action such as **Grant permission**.

Every system request is preceded by app-owned rationale copy. Permanent denial routes to application or channel settings. Returning from system settings refreshes canonical state and completes the still-pending user action when permission is now usable.

## Settings Interaction Design

### Truthful row state

Every visible Settings row is gesture-enabled. A switch represents the persisted requested configuration, not `configured && prerequisitesAvailable`. Separate status copy reports whether the configured feature is currently active, waiting for permission, or waiting for another setting.

Turning a setting off always works immediately. Turning a setting on behaves as follows:

- if prerequisites are usable, commit the setting;
- if runtime permission is missing, open its rationale and request/recovery flow, then commit only after success;
- if another setting is required, retain the requested configuration and expose an explicit prerequisite action.

When a permission is later revoked while a setting remains configured on, the switch stays checked and the row shows **Permission needed** with a **Grant permission** recovery action. This avoids pretending the user's saved choice changed while still making the inactive runtime state clear.

### Dependent privacy settings

Follower detection and Wi-Fi anomaly remain independently configurable. If Phone privacy scan or its runtime permissions are unavailable, their switches still toggle their persisted preferences. Their status reads **Inactive — requires Phone privacy scan** and provides an **Enable phone scan** action that enters the Phone privacy permission flow.

No tap is silently ignored, and no prerequisite is enabled without an explicit action.

### Backend controls

The current backend switch is renamed **Poll configured sensor backend** with copy stating that it applies to the configured Friend or Foe/ESP32 server used by AR, Map, Privacy, health checks, and Calibration.

It remains off when no explicit preference exists. Turning it off immediately cancels configured-backend polling, invalidates backend health, clears only backend-derived live state, and normalizes backend-only mode off. It does not disable or alter:

- public ADS-B aircraft feeds;
- public weather;
- GitHub update checks;
- map tiles or HexDB;
- badge-local access or debug-bridge traffic.

Backend-only mode remains off while configured-backend polling is off, but its row is actionable rather than inert. Attempting to enable it shows a confirmation explaining that it requires configured-backend polling. Only an explicit **Enable polling and backend-only mode** confirmation may enable both. Cancel changes nothing.

The existing fail-closed backend interceptor remains the final guarantee that an off preference produces no configured-backend network request.

## Feature-Surface Recovery

### List

List remains usable without location. When location is unavailable, it shows a non-blocking status/action explaining that nearby distances require foreground location. **Allow location** opens the rationale and canonical request flow. Denial leaves aircraft/drone inventory visible and the recovery action available.

### Calibration

Calibration uses the canonical paired-location request plan and required Bluetooth permissions. Entry shows prerequisite state without trapping the user in an automatic dialog. **Grant permissions** and **Start Walk** both route to the same rationale/request/recovery action.

After denial, Start remains recoverable; permanent denial shows **Open app settings**. Approximate-only location is not sufficient for a calibration walk, but it is preserved while the app explicitly requests the precise upgrade using the fine+approximate pair. Permission revocation during a walk ends protected collection safely and presents a recoverable permission status.

### Privacy and Ultrasonic

Privacy source actions and Ultrasonic use the durable completion coordinator. Granting microphone access turns Ultrasonic on exactly once. Granting radio/privacy permissions completes the requested Phone privacy or source action even if the screen recomposes or navigation changes while Android's dialog is open.

### Camera, notifications, legacy photo saving, and USB

Existing feature-contextual camera and notification flows remain. Permanent notification denial opens app settings; channel blocking opens the exact channel, with app-details fallback if channel settings cannot launch.

On Android 9 and older, photo saving requests legacy storage access when Save is tapped and provides app-settings recovery after permanent denial. Newer Android versions do not request legacy storage.

USB continues using per-device permission. The dynamically registered USB permission receiver is marked not exported on SDKs that require an explicit receiver flag.

## Protected API Boundary Hardening

The release contains permission revocation at the protected call boundary:

- BLE single and batch scan callbacks catch `SecurityException`, publish a permission-blocked source state, and stop unsafe processing.
- Sky notification posting contains `SecurityException` from `notify()` and reports non-delivery without crashing.
- Wi-Fi Aware attach/subscription callbacks contain `SecurityException`, close their sessions/channel, and publish a recoverable permission-blocked outcome.
- Calibration location registration propagates a contained permission-blocked result to the existing walk state.
- Badge GATT read, Privacy notification posting, and Ultrasonic capture keep their existing runtime containment; lint suppression is narrow and documented at the already-contained Android boundary.
- `BleFeatureExtractor` no longer reads `BluetoothDevice.type` as an address type. Address classification is supplied explicitly or remains unknown.

To make Android lint a usable release gate, the implementation also resolves the five current non-permission lint errors without broad refactoring: API-35-gate `BluetoothDevice.getAddressType`, use `BluetoothStatusCodes` for modern GATT write results, mark the USB receiver not exported, and apply the required CameraX transform opt-in. Existing warnings are not part of this change.

## Error and Lifecycle Behavior

- `Loading` is transient and never consumes a pending user action.
- Confirmed denial keeps an in-app retry action.
- Permanent denial opens system settings and retries through lifecycle refresh on return.
- Failure to launch channel-specific settings falls back to application details; failure to launch application details is surfaced in the UI rather than swallowed.
- A permission revoked during collection becomes a paused/permission-needed source state, not an application crash.
- Duplicate Activity Result or lifecycle refresh events cannot commit a setting more than once.
- Cancelling a rationale or backend confirmation leaves settings unchanged.
- Backend polling is never enabled by a permission grant, navigation restore, app resume, or unrelated setting.

## Testing Strategy

### JVM tests

- Exact request plans for SDK 30, 31, 33, and 35, including first location request and approximate-to-precise upgrade.
- Feature requirements for radio/privacy, Ultrasonic, notifications, camera, and Calibration.
- Activity Result completion with a delayed repository refresh and a disposed composition scope; the pending feature resolves in ViewModel scope.
- Ultrasonic pending enable writes `ULTRASONIC -> true` exactly once after `{RECORD_AUDIO: true}` and does not clear on `Loading`.
- Denied and permanently denied results retain the correct retry/settings recovery state.
- Dependent Settings preferences remain independently configurable while runtime status is inactive.
- Backend switch affects only configured-backend state; public service preferences and clients are unchanged.
- Permission revocation tests for BLE callback processing, Sky notification posting, Wi-Fi Aware subscription, Calibration registration, Ultrasonic creation, Privacy notification posting, and Badge GATT containment.

### Compose and navigation tests

- Plain and permission-backed Settings switches invoke their handlers and display the persisted checked state.
- Wi-Fi anomaly and follower rows remain interactive when Phone privacy is unavailable and expose **Enable phone scan**.
- Backend-only mode shows explicit confirmation while backend polling is off; cancel changes nothing and confirmation enables both settings.
- Ultrasonic rationale, grant completion, denial retry, and system-settings recovery render correctly.
- List shows inventory plus a location recovery action rather than silently failing.
- Calibration remains recoverable after denial and provides app-settings recovery after permanent denial.
- About and App settings continue sharing the graph-scoped ViewModel and update state.

### Static, build, and emulator verification

From `android/`:

```bash
./gradlew testDebugUnitTest
./gradlew :app:lintDebug
./gradlew assembleDebug
./gradlew connectedDebugAndroidTest
```

`lintDebug` must have zero errors. Existing warnings may remain.

Fresh-install emulator QA covers:

1. No bulk permission prompt at ordinary launch.
2. All Settings rows respond, with backend polling off.
3. Ultrasonic requests microphone and turns on after Allow.
4. Radio/privacy requests launch the correct location pair and SDK-specific nearby-device permissions.
5. Approximate location can be upgraded through a valid paired request.
6. Denial and permanent-denial recovery remain actionable.
7. List remains useful without location and can request it explicitly.
8. Calibration requests precise location and Bluetooth contextually and can retry after denial.
9. No configured-backend request occurs while its preference is off, while public update/ADS-B behavior remains available.

## Release and Update Delivery

The Android version advances to:

- `versionCode = 118`;
- `versionName = "0.67.9-android-settings-permissions"`;
- tag `v0.67.9-android-settings-permissions`.

The release workflow's exact APK metadata assertion advances with the version. After verification, the branch and tag are pushed. GitHub Actions builds and signs the APK with the established release certificate, publishes the APK plus SHA-256 asset, marks the non-prerelease Android release latest, and generates release notes.

Release verification confirms:

- the workflow succeeded;
- the release is non-draft, non-prerelease, and latest;
- the APK and checksum assets exist;
- the APK package/version/signing certificate match the expected values;
- an installed v0.67.8 RC build reports the new release from About and opens its GitHub release page.

## Scope Boundaries

- Android application code, Android tests, Android workflow version assertions, this spec/plan, and the Android GitHub release only.
- No backend service, database, API, deployment, or configuration changes.
- No ESP32 firmware, build, upload, or hardware changes.
- No startup permission bundle, background-location permission, foreground-service permission, or background worker.
- No change to public online data-provider enablement or routing.
- No redesign of aircraft imagery or About updates beyond preserving their already-restored behavior and verifying update delivery.
- No cleanup of lint warnings after the current lint errors are resolved.

## Acceptance Criteria

- The manifest declares every permission used by the app, with no unnecessary new dangerous permission.
- Ordinary launch requests no sensitive permission.
- Every Settings control responds; no dependency is represented by an inert switch.
- Android 12+ fine-location requests always include approximate location in the same launch.
- Granting microphone from the Ultrasonic setting turns the setting on exactly once and survives UI disposal/recreation.
- List, Calibration, Privacy, camera, notifications, and legacy photo saving have contextual request and permanent-denial recovery paths.
- Revoking a permission during a protected operation produces a recoverable state rather than a crash.
- `Poll configured sensor backend` defaults off and gates only the configured Friend or Foe backend.
- Backend-only mode cannot silently enable backend polling.
- Public ADS-B, weather, GitHub updates, map tiles, and other online services are unaffected by the backend polling switch.
- JVM tests, lint with zero errors, debug assembly, focused connected tests, and available emulator QA pass.
- GitHub publishes `v0.67.9-android-settings-permissions` with the signed APK and checksum as the latest installable Android release.
- The About page on the prior build discovers the release and opens it for installation.

## Self-Review

- The design contains no placeholders or deferred product decisions.
- Permission prompts are contextual and never bundled at launch.
- Configured state and effective runtime state are explicitly distinct.
- Transient `Loading` state cannot destroy pending user intent.
- Backend terminology is limited to the configured Friend or Foe sensor backend and does not imply public online feeds.
- Every identified denial dead end and real revocation race has a recovery or containment rule.
- Release version, tag, metadata assertion, and About update acceptance criteria are exact and mutually consistent.
