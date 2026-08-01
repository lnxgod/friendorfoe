# Task 8 report: enforce Badge transport evidence

## Outcome

Implemented the Android-only Badge transport safety boundary. No file under `esp32/` or
`backend/` was changed, built, flashed, uploaded, or otherwise mutated.

- Added typed transport phases, concrete transport generations, capability evidence,
  commands, acknowledgements, outcomes, and a `BadgeControlPort` owned by the process
  lifecycle.
- Added clock-driven freshness: USB/AP/debug become stale at 10 seconds, BLE at 20
  seconds, and all verified state expires at 60 seconds. Disconnect/error remains
  truthful while cached configuration and detections expire independently.
- Kept checked-in mutation certification empty. Runtime transport discovery never adds
  certification; physical command certification remains Task 18 work.
- Made USB, AP, BLE, and debug execution one-shot and fail-closed. USB/BLE timeout,
  cancellation after an attempted write, or uncertain USB transfer quarantines that
  concrete transport until reconnect. There are no automatic command retries.
- Split HTTP status and command clients. Status uses 1.2-second read/1.5-second call
  limits; commands use six seconds; both disable retry-on-connection-failure.
- Made acknowledgement parsing presence-aware and strict. Non-2xx, malformed/missing
  `ok`, `ok:false`, `applied:false`, wrong primitive types, unknown modes, and invalid
  unsigned hashes fail. Recovery accepts only the exact raw `FOF_REBOOT:OK` or
  `FOF_BOOTLOADER:OK` line for its matching pending direct-USB command.
- Preserved a factual exact recovery acknowledgement across the badge's expected
  disconnect only when the same target/generation remains disconnected and no new
  transport has won. Recovery always shows reconnect guidance.
- Added process/session, active transport, concrete USB/GATT instance, command, BLE
  operation epoch, and physical debug-target guards. Stale callbacks, polls, permission
  results, detach events, and command completions cannot overwrite a newer winner.
- Serialized BLE GATT setup and use as MTU request -> service discovery -> CCCD write ->
  status read. A control write waits for the exact GATT epoch to become idle; reset wakes
  but rejects stale waiters.
- Made USB detach match the exact active physical target and made permission broadcasts
  match the issuing session, target, and nonce. USB writes hold the same generation gate
  from final identity check through `bulkTransfer`.
- Made HTTP discovery token-bound before the request. Expired or unverified HTTP leases
  release so AP/debug/BLE fallback remains possible, while late responses cannot reclaim
  a new generation. Status requests now have newest-request publication and compare-and-swap
  generation replacement, so a slow debug target A cannot reclaim after a newer target B.
- Linearized HTTP mutation start at non-blocking OkHttp `enqueue` while holding status-request
  then transport authority. An in-flight status request blocks command start, and the exact
  published target/token is rechecked at that final point without holding a general lock over
  network I/O.
- Linearized BLE scan start against synchronous session invalidation and replaced loose scan
  flags with an identity-bound lease. Callback, timeout, and lifecycle stops retain that exact
  lease through the platform stop attempt; stale work cannot clear a newer scan.
- Added a debug-build-only bridge URL with an emulator default and Gradle-property
  override. Release emits an empty URL and cannot poll or execute the bridge. Debug
  recovery remains unsupported.
- Removed Android firmware picker/upload/relay/CRC behavior while preserving read-only
  scanner fields `fw_state`, `target_ver`, `ota_state`, and `last_fw_error`.
- Moved Badge start/stop to `FriendOrFoeApplication` process lifecycle, removed screen
  Badge lifecycle ownership, retained List's location lifecycle, and removed fabricated
  `(0.0, 0.0)` sky starts.
- Stored elapsed and wall receipt timestamps together at each USB/BLE/HTTP boundary.
  Privacy mapping now uses that immutable receipt and cannot rejuvenate cached rows.
- Replaced the unrelated `URLEncoder` route helper with a byte encoder that preserves its
  prior output exactly, because the authoritative Android firmware-removal scan requires
  zero `URLEncoder` matches.

## TDD evidence

The first focused RED failed at `compileDebugUnitTestKotlin` because the requested
connection, capability, command, and evidence APIs did not exist.

Additional failing-first regressions reproduced and fixed:

- timeout/cancellation versus late USB acknowledgement and BLE callback quarantine;
- same-transport concrete instance replacement and stale command publication;
- exact active-target detach and stale permission result across stop/start;
- same-target reopen/permission state retaining unverifiable cached configuration;
- HTTP expiry/unverified lease blocking fallback and late AP publication;
- debug physical target A -> B reusing A's command generation;
- live snapshot A authorizing a replacement token B;
- exact recovery acknowledgement being lost on its expected detach;
- BLE read/control overlap, setup-operation overlap, and stale waiter reacquisition after
  a GATT reset;
- AP/debug POST initiation after stop, direct-USB takeover, or physical-target replacement;
- slow target-A debug status reclaiming after a fast target-B request;
- BLE scan starting after stop, duplicate concurrent scan starts, stale timeout clearing a
  newer lease, and lifecycle return before the exact platform stop attempt;
- status receipt wall-clock defaults and Privacy remap rejuvenation;
- legacy route encoding of spaces, `*`, `~`, slash, and Unicode.

The concurrency fix round had four explicit RED checkpoints:

- missing HTTP command/status and BLE scan coordinators failed test compilation;
- missing compare-and-swap transport replacement failed test compilation;
- missing active-status and lease-held stop APIs failed test compilation; and
- a stopped HTTP request gate incorrectly authorized a command start, producing one expected
  behavioral test failure before the session-active guard was added.

An independent post-fix concurrency review found no remaining Android-formal blocker or lock
cycle. A bridge serial switch after Android has atomically enqueued a command cannot be bound
without an expected-serial compare-and-swap in the bridge API; this remains a physical
certification boundary, and checked-in debug mutation certification is therefore still empty.

## Fresh verification

- Task-focused Badge command:
  `./gradlew testDebugUnitTest --tests '*BadgeConnectionCapabilityTest' --tests '*BadgeControlAcknowledgementTest' --tests '*BadgeUsbLineParserTest'`
  - Result: `BUILD SUCCESSFUL`
  - Count: 55 tests, 0 skipped, 0 failures, 0 errors
- Expanded focused command also included `BadgePrivacyMapperTest` and `RouteCodecTest`.
  - Result: `BUILD SUCCESSFUL`
  - Count: 64 tests, 0 skipped, 0 failures, 0 errors
- Race-focused command:
  `./gradlew testDebugUnitTest --tests com.friendorfoe.data.badge.BadgeTransportRaceCoordinatorTest`
  - Result: `BUILD SUCCESSFUL`
  - Count: 13 tests, 0 skipped, 0 failures, 0 errors
- Full Badge package command included every `com.friendorfoe.data.badge.*` test.
  - Result: `BUILD SUCCESSFUL`
  - Count: 99 tests, 0 skipped, 0 failures, 0 errors
- Full JVM command: `./gradlew testDebugUnitTest --rerun-tasks`
  - Result: `BUILD SUCCESSFUL in 28s`
  - Count: 375 tests, 0 skipped, 0 failures, 0 errors
- APK command: `./gradlew assembleDebug --rerun-tasks`
  - Result: `BUILD SUCCESSFUL in 31s`
  - 41 tasks executed
- Release BuildConfig proof:
  - `DEBUG = false`
  - `BADGE_DEBUG_BRIDGE_BASE_URL = ""`
- Debug override proof with
  `-PbadgeDebugBridgeUrl=http://127.0.0.1:8765/` generated that exact debug URL; the
  default debug BuildConfig was regenerated afterward.
- The authoritative broad firmware-mutation regex has zero matches across
  `android/app/src/main`, `android/app/src/test`, and `android/app/src/androidTest`.
- Badge lifecycle/fabricated-coordinate scan: zero matches in Application, List, and
  Privacy.
- All three production status-parser boundaries pass one captured elapsed receipt and
  one captured wall receipt explicitly; no wall-clock default remains.
- `git diff --check`: clean.
- Backend/firmware scope scan: no `backend/` or `esp32/` diff.

Build output still contains repository-existing AGP/compileSdk, Room schema-export,
deprecation, and unused-symbol warnings. The final fresh build contains no new Badge
warning category.

## Files changed

Production/configuration:

- `android/app/build.gradle.kts`
- `android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeBleGattOperationCoordinator.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeCommand.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeCommandEvidence.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeConnection.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeControlPort.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeDebugBridgeConfig.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeHttpClients.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeReleaseCertification.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeStatusModels.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeStatusParser.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeTransportRaceCoordinators.kt`
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- `android/app/src/main/java/com/friendorfoe/data/time/MonotonicClock.kt`
- `android/app/src/main/java/com/friendorfoe/di/ApplicationCoroutineModule.kt`
- `android/app/src/main/java/com/friendorfoe/di/BadgeModule.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/navigation/RouteCodec.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`

Tests:

- `android/app/src/test/java/com/friendorfoe/data/badge/BadgeConnectionCapabilityTest.kt`
- `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlAcknowledgementTest.kt`
- `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlStatusParserTest.kt`
- `android/app/src/test/java/com/friendorfoe/data/badge/BadgeTransportRaceCoordinatorTest.kt`
- `android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbLineParserTest.kt`
- `android/app/src/test/java/com/friendorfoe/presentation/navigation/RouteCodecTest.kt`
- `android/app/src/test/java/com/friendorfoe/presentation/privacy/BadgePrivacyMapperTest.kt`
- `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyBackendPollingTest.kt`

## Deferred, non-blocking concerns

- No physical Badge or Android phone was required for this JVM/build gate. Consequently,
  checked-in mutation certification remains intentionally empty and the release app will
  not send a mutation. Task 18 must record physical evidence before enabling any exact
  transport/command pair.
- List/Privacy configuration surfaces remain temporarily connected to the process-scoped,
  typed, fail-closed port as planned; Task 10 owns their final consolidation/removal.
- No bridge script, backend endpoint, or firmware behavior was changed.
