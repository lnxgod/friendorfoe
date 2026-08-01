# Task 3: Host-Aware USB Recovery Report

## Scope and starting point

- Worktree: `/Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final`
- Branch: `codex/defcon34-badge-final`
- Required base and starting `HEAD`: `702fc578af25901ecfca7920e09af73bd310d098`
- Pre-existing protected untracked file: `.camera-before-zoom.jpg` (not read, edited, staged, or committed)
- No firmware version was changed. No hardware was flashed. No release, factory, signing, push, or tag action was performed.

## Baseline evidence

Before implementation:

```text
cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test
525 test cases: 525 succeeded
```

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_quiet_mode_contract.py -v
9 passed in 0.03s
```

## RED evidence

The runtime-policy tests and source contracts were extended before implementation.

```text
cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test
```

Expected compile RED: 11 errors for the absent one-boot recovery API and rollback-health API, including `BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB`, `BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR`, `badge_runtime_recovery_token_decide`, and `badge_runtime_rollback_health_satisfied`. PlatformIO reported the native environment errored after 1.131 seconds.

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_quiet_mode_contract.py -v
```

Expected source-contract RED: `4 failed, 8 passed in 0.14s`. Failures identified the missing host-aware chord/recovery helper, persistent forced-safe NVS behavior, and rollback validation without completed post-boot response proof/status reason.

Follow-up contracts also failed before their corresponding corrections:

- ROM recovery drain failure still selected the app fallback, expected reboot reason was erased/qualified by the immediate reset class, and display health was recorded unconditionally: `3 failed, 9 passed in 0.09s`.
- Relay progress used a C11 64-bit atomic instead of the existing Xtensa-safe critical-section pattern: the focused contract failed `1 failed` before the lock conversion.

## Implementation

- Added one unified `_Noreturn` USB recovery helper for application and ROM resets.
- The ten-second chord retains the exact 10,000 ms policy, consumes both releases, samples the public SOF-based USB host API for up to 25 ms, and requires a fresh debounced OK confirmation after release before ROM entry. No host, Menu, both buttons, or timeout selects an application restart.
- ROM entry performs a bounded best-effort TX drain. A drain failure is logged but never blocks `RTC_CNTL_FORCE_DOWNLOAD_BOOT`, preserving physical recovery when the USB transport itself is silent or failed.
- Routed `FOF_BOOTLOADER` and `FOF_REBOOT` through the unified helper after their required response succeeds.
- Replaced persistent forced-safe NVS state with a reset-cause-qualified, consumed RTC one-boot token. Expected reboot reason is stored independently in NVS and reported by `FOF_STATUS` across ROM flashing/re-enumeration; `last_reset_expected` remains a separate reset-cause fact.
- Replaced the old USB liveness watchdog branch with `badge_usb_health_decide()`, including active upload and scanner-relay progress. The automatic `usb_safe_once` restart is single-shot.
- Rollback validation now requires real display power and a completed post-boot `FOF_PING` or `FOF_STATUS` response. Normal boot additionally requires both UART worker tasks to heartbeat; safe USB intentionally does not require scanner identity, version, connection, or radio health.
- Started the independent button task immediately after display initialization and before event-loop, UART, network, or scanner work, including when panel initialization fails.
- Relay progress timestamps use the existing `portMUX_TYPE` critical section, avoiding 64-bit Xtensa atomic/linkage concerns.

## GREEN evidence

```text
cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test
530 test cases: 530 succeeded in 0.887s
```

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_quiet_mode_contract.py \
  backend/tests/test_badge_firmware_transport_contract.py -q
108 passed in 0.27s
```

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
439 passed in 1.56s
```

Clean build:

```text
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
1 succeeded in 0.493s

/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
1 succeeded in 40.519s
RAM:   173508 / 327680 bytes (53.0%)
Flash: 1204441 / 2097152 bytes (57.4%)
FoF: badge uplink flash manifests and referenced paths verified
```

One initial clean invocation was made from `esp32/`, where only the native `test` environment exists; PlatformIO rejected `uplink-s3-fof_badge` with `UnknownEnvNamesError`. It was immediately rerun from the correct `esp32/uplink/` project and succeeded as shown above.

## Post-commit audit correction

A final audit after the primary Task 3 commit found that the scanner post-update
health proof may legitimately wait for 180 seconds, while its last emitted relay
progress timestamp could become older than the 90-second USB watchdog limit.
The prior `badge_runtime_note_usb_control_alive()` call did not feed the new
policy snapshot, so the watchdog could falsely restart during a healthy relay.

A new RED contract required the 180-second loop to refresh the dedicated relay
progress timestamp before each 500 ms delay. It failed against the prior code;
after implementation, the older transport contract also failed once because it
still required the obsolete USB-control heartbeat. Both contracts now require
the policy-visible relay-progress refresh.

The implementation centralizes timestamp updates in
`fw_store_note_relay_progress()`, protected by the existing `portMUX_TYPE`
critical section. Both emitted relay progress and every iteration of the long
post-update health loop use it.

Post-correction evidence:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_quiet_mode_contract.py \
  backend/tests/test_badge_firmware_transport_contract.py -q
108 passed in 0.39s

/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
439 passed in 1.88s

cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
1 succeeded in 0.447s

/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
1 succeeded in 41.216s
RAM:   173508 / 327680 bytes (53.0%)
Flash: 1204453 / 2097152 bytes (57.4%)
FoF: badge uplink flash manifests and referenced paths verified
```

The native suite was not rerun for this focused correction because no shared or
native-policy source changed; the immediately preceding 530/530 native result
remains the native evidence for Task 3.

## Physical validation limits

No connected badge was flashed or operated. The following remain explicitly unverified on physical hardware:

- exact ten-second two-button chord behavior and release consumption;
- data-host versus power-only charger SOF discrimination;
- Android-host second-confirmation behavior;
- the on-panel `USB FLASH? / OK=YES / MENU=RESET` prompt and five-second timeout;
- ROM download-mode enumeration after either the chord or `FOF_BOOTLOADER`;
- recovery when the USB transport lock/driver is actually unavailable;
- one-boot safe-USB behavior across a real reset and subsequent repair;
- OTA pending-verify validation/rollback behavior across real boots;
- scanner UART worker behavior with blank, disconnected, or broken scanner hardware.

## Final post-audit edge hardening

The final Task 3 audit identified four recovery-path gaps that were not safe to
leave for hardware acceptance:

- a persistent USB transport initialization failure could continue into normal
  startup after the one-shot recovery token had been consumed;
- button and display worker allocation still depended on the heap exactly when
  those workers were needed as the last local recovery surface;
- the display worker could overwrite the recovery screen before startup had
  proved all required workers;
- the USB watchdog sampled firmware inactivity and restarted in two separate
  critical sections, allowing a firmware operation to begin between them.

The correction followed a strict RED/GREEN cycle. New and updated source
contracts initially produced the expected result:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_quiet_mode_contract.py \
  backend/tests/test_badge_firmware_transport_contract.py \
  backend/tests/test_firmware_build_version.py -q
5 failed, 134 passed in 0.58s
```

After implementation and contract adaptation, the same focused gate passed:

```text
139 passed in 0.42s
```

The badge now keeps byte-sized, 16-byte-aligned static FreeRTOS TCB and stack
storage for both physical buttons and the display. Compile-time assertions
prove the ESP-IDF `StackType_t` sizing assumption. Button initialization is
idempotent and returns a checked success value. The display task is created
before scanner/network/event-loop startup but blocks on a notification; only a
normal startup that has created every required worker releases it. Startup
recovery-only paths preserve each recovery surface whose own dependency
initialized, without starting the live display loop. This is deliberately not
a claim that every failure retains all three surfaces: a failed USB transport
cannot provide USB, a failed button task cannot provide button input, and a
failed display task cannot provide the live display worker. The remaining
successfully initialized surfaces stay available so the badge fails as usefully
as the failed dependency permits.

The first USB transport start failure still arms the one-shot expected restart.
If the recovery token was already consumed and transport initialization still
fails, startup now forces recovery-only with the exact reason
`usb_transport_init` and returns before event-loop/scanner/network work.

Firmware restart exclusion is now atomic. A watchdog restart reserves the
restart under the existing firmware-operation critical section only when no
operation is active. Once reserved, `operation_try_begin()` refuses new upload
or relay work. The reservation has intentionally no clear path because every
successful reservation is consumed by a non-returning restart. The badge
low-heap restart uses the same reservation.

Final automated evidence:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
442 passed in 1.82s

cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
532 test cases: 532 succeeded in 1.168s

cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
SUCCESS
RAM:   190612 / 327680 bytes (58.2%)
Flash: 1206769 / 2097152 bytes (57.5%)

/Users/billh/.platformio/penv/bin/pio run -e uplink-s3
SUCCESS
RAM:   104432 / 327680 bytes (31.9%)
Flash: 1239241 / 2097152 bytes (59.1%)

python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv
badge uplink manifests: strict verification passed
sha256 firmware.bin af916048632cb5be91c794d765479a44e31ccd81bc908c88c7796ad71a3c2b31

git diff --check
PASS
```

No badge was flashed and no hardware, version, factory-image, push, tag, or
release action was performed in this correction.

## Final automatic-restart ownership correction

A final cumulative review found three post-dispatch automatic restart gaps:

- required badge worker creation failure restarted directly after USB dispatch
  had opened;
- scanner-update coordinator restore failure restarted directly in the same
  post-dispatch window;
- the backend-enabled badge low-heap branch still sampled relay state before a
  direct restart, while only standalone badge mode used the atomic reservation.

The strict RED gate added independent contracts for the shared helper and each
of the three affected behaviors. Before production changes, the exact result
was:

```text
4 failed, 99 deselected in 0.33s

test_badge_automatic_restart_helper_waits_for_firmware_ownership
test_badge_required_worker_failure_defers_automatic_restart
test_badge_coordinator_restore_failure_defers_automatic_restart
test_both_badge_low_heap_modes_defer_automatic_restart
```

Badge-only automatic failures now enter a `_Noreturn` helper that retries the
existing atomic restart reservation every 250 ms. It logs the defer once and
allocates no task, queue, timer, or heap storage. An active USB stage or scanner
relay is allowed to finish or abort; the first subsequent reservation win
prevents any new firmware operation before rollback/restart. All reservation
call sites execute on `app_main`, and every successful reservation immediately
enters a non-returning restart, so a reachable failed helper attempt represents
an owned firmware operation rather than a second restart holding a stale
reservation. Explicit button- and host-requested resets remain immediate.
Non-badge low-heap behavior remains guarded by the existing relay-active check.

Final GREEN evidence for this correction:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_quiet_mode_contract.py \
  backend/tests/test_badge_firmware_transport_contract.py \
  backend/tests/test_firmware_build_version.py -q
143 passed in 0.40s

/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
446 passed in 1.82s

cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
532 test cases: 532 succeeded in 1.061s

cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
SUCCESS
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
SUCCESS
RAM: 190612 / 327680 bytes (58.2%)
Flash: 1206877 / 2097152 bytes (57.5%)

/Users/billh/.platformio/penv/bin/pio run -e uplink-s3
SUCCESS
RAM: 104432 / 327680 bytes (31.9%)
Flash: 1239241 / 2097152 bytes (59.1%)

python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv
badge uplink manifests: strict verification passed
sha256 firmware.bin 9b849856cfe3fdfc0c94896c13b8208f5fda761d89c2bacee04112f74eab16e2

git diff --check
PASS
```

No hardware was flashed or operated, and no version, factory-image, push, tag,
or release action was performed for this correction.

## Final repeatable-failure safe-USB escalation

The final cumulative audit found that repeatable automatic failures were
classified as expected software restarts. That kept the crash counter clean,
but it also allowed a required-worker, coordinator-restore, or automatic
low-heap failure to repeat forever without entering the minimal recovery
surface. Two returned-failure edges in the ESP-IDF rollback API had the same
problem when no usable fallback partition was available.

Strict TDD captured three RED stages before the corresponding production
edits:

```text
5 failed, 103 deselected in 0.35s

test_badge_repeatable_automatic_failure_arms_one_boot_usb_recovery
test_badge_pending_verify_rollback_does_not_arm_safe_usb_token
test_consumed_automatic_recovery_token_forces_recovery_only_with_reason_precedence
test_consumed_automatic_recovery_token_is_applied_before_usb_dispatch_and_services
test_automatic_safe_escalation_does_not_change_intentional_reset_token_policy

1 failed, 108 deselected in 0.26s
test_badge_failed_pending_verify_rollback_arms_safe_token_before_fallback_restart

1 failed, 109 deselected in 0.26s
test_badge_unhealthy_pending_boot_failed_rollback_enters_one_shot_usb_recovery
```

After the atomic firmware restart reservation succeeds, a validated badge
image now arms the existing RTC one-boot USB recovery token before its
automatic expected restart. A pending-verify image does not arm the token
before attempting rollback, so a successful rollback boots the prior image
normally. If either automatic rollback call unexpectedly returns because no
fallback slot can boot, the badge arms/enters the one-shot recovery path
instead of retrying forever.

On the following expected software boot, a consumed token sets
`badge_startup_recovery_only` before the transport is placed in recovery-only
mode and before dispatch, event-loop, scanner, or network work. The LCD,
buttons, and minimal USB allowlist remain available. Its visible safe reason is
`usb_safe_once` unless an earlier startup dependency already recorded a more
specific reason. The automatic restart continues to persist its specific
expected reboot reason. Explicit button/host resets remain immediate and use
their existing token policy. Non-badge behavior is unchanged.

All three `esp_ota_mark_app_invalid_rollback_and_reboot()` call sites were
audited. The two automatic call sites in `main.c` receive returned-failure
escalation. The third is the acknowledged explicit serial rollback in
`serial_config.c`; its `usb_rollback` expected-reason and wire behavior were
left unchanged in this task.

Final GREEN evidence:

```text
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_quiet_mode_contract.py \
  backend/tests/test_badge_firmware_transport_contract.py \
  backend/tests/test_firmware_build_version.py -q
150 passed in 0.21s

/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
453 passed in 1.73s

cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
532 test cases: 532 succeeded in 0.913s

cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
SUCCESS
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
SUCCESS
RAM: 190612 / 327680 bytes (58.2%)
Flash: 1206957 / 2097152 bytes (57.6%)

/Users/billh/.platformio/penv/bin/pio run -e uplink-s3
SUCCESS
RAM: 104432 / 327680 bytes (31.9%)
Flash: 1239241 / 2097152 bytes (59.1%)

python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv
badge uplink manifests: strict verification passed
sha256 firmware.bin cd81f08bfcfa95aec3fd65af33d55303113b6b289959015eb6a8209111ff5719

git diff --check
PASS
```

No badge was flashed or otherwise operated, and no progress-ledger, version,
factory-image, push, tag, or release action was performed for this correction.
