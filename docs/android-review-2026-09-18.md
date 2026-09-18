# Android reliability and usability review — September 18, 2026

This pass reviewed update delivery, privacy encounter evidence, and recorded
flight-path interaction against the published 0.67.19 app. Changes ship in
0.67.20-android-review, Android version code 128.

## Findings addressed

### App updates could lead to a release with no Android download

The repository publishes Android, firmware, and dashboard releases. The old
checker used GitHub's repository-wide latest-release endpoint and offered its
release page without checking for an APK. It now selects the newest eligible
version on a page of 100 releases, with up to two additional pages when needed
because no Android build is present. Drafts, prereleases, incomplete uploads,
and releases without the exact version-matched APK are excluded. Asset URLs
must match the official repository and release. Missing eligible builds produce
a retryable check failure, rather than falsely saying the installed app is current.

The update row opens the APK download directly. A separate Release notes action
opens the corresponding release page. Android still handles the download and
installation confirmation; the app does not silently install anything.

### Delayed privacy observations acquired newer-looking timestamps

Encounter timestamps previously used processing time even when the source's
elapsed timestamp indicated an older observation. The log now subtracts that
age when calculating the displayed/copyable wall-clock time. Stable encounter
ordering uses the first observed elapsed timestamp, so a phone clock adjustment
does not reorder rows. Existing session-only storage, expiry, clear behavior,
ignored-device exclusion, and source identity separation are unchanged.

### Flight-path inspection needed precise stepping and a return to latest

Previous and Next inspect individual received positions without needing to drag
the slider precisely. Latest position resumes following the newest received
position and fits the map to the current path. An explicit status distinguishes
reviewing a saved point from following new data. The slider exposes its selected
timestamp to accessibility services. New observations do not disturb a point
that the user is currently reviewing.

## Verification

- JVM tests cover mixed release types, numeric version ordering, pagination,
  malformed URLs, incomplete assets, cancellation, and actual GitHub JSON
  conversion through Retrofit, as well as delayed privacy timestamps and clock
  changes. The full 1,103-test suite passes.
- 18 API 35 emulator tests cover direct APK and release-note actions, About
  states, navigation, privacy evidence, and flight-path selection during live
  data changes. Screenshots were inspected for the changed controls.
- Android lint completes with no errors.

## Scope remaining

This is a targeted improvement pass, not an exhaustive audit of every detector
or a physical radio test. Google Play preparation remains separate: the app
still targets API 34 and the current release workflow publishes a signed APK.
