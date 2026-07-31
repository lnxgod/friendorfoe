# Badge Updater Bounded Retries Implementation Plan

> **Execution:** Work sequentially in the current private task. Use
> test-driven development for every behavior change and stop before physical
> flashing until the operator authorizes attended acceptance.

**Goal:** Make the one-uplink USB updater automatically recover the transient
scanner readiness, OTA acknowledgement, manifest-backoff, and stale
maintenance-session failures reproduced during the `.92` canary, while
preserving every existing identity and health gate.

**Architecture:** Keep the current combined scanner campaign as the first
attempt. Capture a typed, immutable campaign failure instead of immediately
flattening it into `FlashError`. Restore the exact uplink to proven normal mode,
classify only known transient scanner evidence, reboot only the failed scanner,
and run a new one-lane maintenance campaign. Accumulate verifier-issued attempt
evidence across at most three campaigns per lane. A descriptor-bound,
non-writing USB reset is available once only when the owned maintenance session
cannot otherwise return to normal.

**Tech stack:** Python 3 standard library, `unittest`, pyserial, guarded
esptool 4.11.0, existing ESP32 application USB/UART protocol, and existing
source-contract hardening verifier.

## Global Constraints

- Modify host-side updater and verifier code only.
- Do not modify any file under `esp32/uplink/`, `esp32/scanner/`,
  `esp32/shared/`, or `android/`.
- Do not build, flash, reboot, reset, or otherwise mutate connected hardware
  during implementation and automated verification.
- Do not change the CON CRUD game, badge UI, detectors, themes, Easter eggs,
  factory firmware bundle, Android app, or release metadata.
- Keep `0.64.92-badge-defcon34` as the private canary identity.
- Reserve `0.67.0-badge-defcon34` until separate attended physical, game, and
  Android acceptance.
- Keep the current combined first campaign.
- Permit at most three host-level campaigns per requested lane, including the
  first campaign.
- Never restage or rewrite a lane already proven `converged` or `current`.
- Use the exact same frozen scanner bytes, target, digest, size, uplink binding,
  and original scanner identities for every retry.
- Preserve the configured game seed/state across every maintenance cleanup and
  retry.
- Keep unknown errors and all identity, role, artifact, downgrade, partition,
  rollback, crash, safe-mode, and health inconsistencies terminal.
- Preserve the unrelated untracked `.camera-before-zoom.jpg`.
- Commit locally only. Do not push, tag, merge, release, or replace public
  assets.

---

## Task 1: Add Typed, Immutable Scanner Campaign Failure Evidence

**Files:**

- Modify: `scripts/fof_badge_flash.py`
- Test: `scripts/test_fof_badge_flash.py`

### Step 1: Write failing campaign-evidence tests

Add tests beside the existing compact maintenance campaign tests proving:

- `failed` after zero attempts and exhausted readiness produces a typed
  campaign failure after the existing single `fw_check_now` grace expires;
- `failed` after one or more relay attempts produces the same typed failure;
- the failure contains defensive copies of the exact stage receipt and compact
  campaign snapshot;
- its failed, successful, and requested slot sets are immutable and derived
  only from validated campaign state;
- `refused` and `newer_skipped` remain terminal ordinary `FlashError` cases;
- stage generation, slot mask, digest, size, or committed-byte drift remains
  terminal and cannot create retry evidence.

Run the focused tests and confirm RED:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest \
  scripts.test_fof_badge_flash.UpdateMaintenanceTransactionTests.test_zero_attempt_campaign_exposes_typed_retry_evidence \
  scripts.test_fof_badge_flash.UpdateMaintenanceTransactionTests.test_attempted_campaign_exposes_typed_retry_evidence \
  -v
```

Expected: tests fail because no typed campaign failure exists.

### Step 2: Implement the typed failure boundary

Add a private `ScannerCampaignFailure(FlashError)` whose constructor validates
and JSON-freezes:

- maintenance session;
- requested slots;
- stage receipt;
- compact campaign snapshot;
- failed slots;
- slots already in `converged` or `current`;
- per-slot attempt counts and readiness probes.

Expose only defensive-copy or immutable properties. Do not accept caller-built
success authority without passing the current strict status validators.

Change `_wait_for_maintenance_scanner_campaign` so only a validated scanner
`failed` terminal state produces this type. Preserve the existing targeted
zero-attempt reprompt before constructing the failure.

### Step 3: Add and test strict transient classification

Write failing table-driven tests for a pure
`_classify_scanner_campaign_retry(...)` helper. It consumes the typed campaign
failure plus a validated normal-mode status and original scanner identities.

Accept only:

- `readiness_exhausted` for a zero-attempt exhausted lane;
- `ota_ack_timeout` from that lane's exact relay/firmware error fields;
- `offer_manifest_mismatch`;
- `deferred_backoff` when `fw_state == "deferred"` and the same lane reports a
  positive firmware backoff;
- a bounded temporary absence only when the original MAC is not replaced and
  UART evidence still identifies the expected physical lane.

Reject:

- an unknown error string;
- conflicting error fields;
- multiple ambiguous failed lanes;
- changed or duplicate scanner MAC;
- wrong slot role/profile;
- current/newer/downgrade ambiguity;
- any unsafe, rollback, crash, or radio-health regression.

Implement the helper and rerun the focused class tests until GREEN.

### Step 4: Commit

```sh
git add scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py
git commit -m "tools: classify retryable scanner campaigns"
```

---

## Task 2: Return Proven Normal Recovery and Add One Non-writing USB Reset

**Files:**

- Modify: `scripts/fof_badge_flash.py`
- Test: `scripts/test_fof_badge_flash.py`

### Step 1: Write failing maintenance-recovery tests

Extend the existing `_recover_failed_update_maintenance` tests to require:

- successful automatic normal reconnection returns a validated recovery
  snapshot instead of always throwing;
- exact-session abort returns a validated normal snapshot;
- a lost abort receipt reconciles normal mode without losing the original
  diagnostic note;
- an abort session conflict or stale owned maintenance marker can request one
  descriptor-bound USB-reset fallback;
- the fallback is unavailable for a wrong session, wrong descriptor,
  wrong physical location, or wrong MAC;
- a second fallback request in one invocation is refused;
- malformed normal status and changed game state remain terminal;
- callers that are not scanner-retry coordinators still rethrow their original
  failure after cleanup.

Run the focused recovery class and confirm RED.

### Step 2: Implement a returnable recovery result

Add an immutable private recovery result containing:

- validated normal-mode status;
- rebound `UsbDescriptorRecord`;
- cleanup action (`already_normal`, `session_abort`, or `usb_reset`);
- whether the one-reset budget was consumed;
- secondary diagnostic notes.

Refactor `_recover_failed_update_maintenance` into a primitive that returns this
result on proven cleanup. Keep a small wrapper for existing entry/uplink failure
paths that performs cleanup and then rethrows their primary exception.

### Step 3: Implement the guarded non-writing reset

Reuse only existing guarded esptool builders and parsers:

1. Close the application serial transport.
2. Census the same descriptor serial and trusted physical location.
3. Run `reset_uplink_usb_to_rom` with native USB reset.
4. Require its ROM base MAC to equal the bound uplink hardware ID.
5. Run `build_esptool_run_argv` through `run_guarded_esptool`.
6. Validate the run receipt with `parse_esptool_run_result`.
7. Reconnect and strictly prove the same normal application and game state.

The reset path performs no erase, write, verify-write, partition mutation, or
artifact access. Guard it with one invocation-local exact boolean budget.

### Step 4: Run recovery and guarded-esptool tests

```sh
python3 -m unittest \
  scripts.test_fof_badge_flash.UpdateMaintenanceTransactionTests \
  scripts.test_fof_badge_flash.GuardedEsptoolRunnerTests \
  -v
```

Expected: all selected tests pass.

### Step 5: Commit

```sh
git add scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py
git commit -m "tools: recover owned maintenance sessions safely"
```

---

## Task 3: Recover the Exact Scanner and Retry Only Unresolved Lanes

**Files:**

- Modify: `scripts/fof_badge_flash.py`
- Test: `scripts/test_fof_badge_flash.py`

### Step 1: Write failing exact-lane recovery tests

Add tests for a `BadgeSerial.recover_scanner_lane(...)` operation proving it:

- accepts only `ble` or `wifi` and one normalized expected scanner MAC;
- sends exactly
  `{"cmd":"scanner_recovery","uart":"<lane>","enabled":false}`;
- requires the existing receipt message, only the selected `*_sent` field,
  `enabled == false`, and `reboot_required == true`;
- refuses `all`, an unaccepted send, malformed receipt, or peer-lane send;
- waits for the same scanner MAC, role, profile, normal recovery state,
  rollback clearance, and healthy radio to reannounce;
- never accepts a replacement scanner or silently switches lanes.

Confirm the tests fail before adding production behavior.

### Step 2: Add failing coordinator tests

Build deterministic fake-badge scenarios for:

1. Combined BLE+Wi-Fi attempt succeeds: one stage, no recovery, no reset.
2. BLE converges while Wi-Fi gets `ota_ack_timeout`: preserve BLE, reboot and
   retry only Wi-Fi, two total stages.
3. Wi-Fi converges while BLE reports manifest backoff: preserve Wi-Fi and retry
   only BLE.
4. First retry fails transiently and second retry succeeds: three total
   campaigns for that lane.
5. Third campaign fails: stop with complete attempt history and no fourth
   mutation.
6. A terminal invariant failure after cleanup: no scanner recovery or retry.
7. A successful peer regresses identity/version/health during recovery: stop.
8. Game seed/state changes during recovery: stop.
9. Stale maintenance uses the one USB reset and then completes the exact-lane
   retry.
10. Both lanes fail ambiguously: stop rather than guessing an order.

Each scenario must assert exact maintenance session ordering, stage slot masks,
scanner recovery commands, and final status calls.

### Step 3: Implement the bounded retry coordinator

Keep the current first attempt in `_usb_update_maintenance_flow`. Add a private
scanner retry coordinator invoked only when that attempt raises the typed
campaign failure.

For each retry:

- recover the owned maintenance session to validated normal;
- classify against fresh normal scanner status;
- verify all successful peer lanes against original identities and target;
- enforce the three-campaign per-lane budget;
- reboot only the classified failed lane;
- wait for the same scanner to become healthy;
- create a fresh maintenance session;
- prepare and prove maintenance on the same uplink;
- stage the same frozen artifact with only the unresolved lane in its slot
  mask;
- run the existing strict coordinator wait;
- finish maintenance and reprove normal mode.

Do not retry uplink OTA. On later attempts the already-updated uplink must be
treated as current and its expected partition/version must remain unchanged.

Use the existing global transfer deadline. Do not create an unbounded
per-attempt timeout reset.

### Step 4: Separate final scanner health from latest campaign proof

After a multi-attempt success:

- validate the latest exact stage receipt and convergence only against the
  latest attempt's slot mask;
- validate version, identity, role, rollback, crash, and radio health for every
  originally requested scanner;
- validate stored prior-attempt snapshots as the convergence authority for
  peer lanes completed in earlier campaigns;
- rerun those same checks after reversible theme control.

This prevents the latest one-lane coordinator generation from erasing proof of
the peer lane completed by the first combined campaign.

### Step 5: Run updater tests

```sh
python3 -m unittest scripts.test_fof_badge_flash -v
```

Expected: the full updater suite passes with no hardware access.

### Step 6: Commit

```sh
git add scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py
git commit -m "tools: retry failed scanner lanes automatically"
```

---

## Task 4: Attest and Verify Multi-attempt History

**Files:**

- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/verify_badge_usb_hardening.py`
- Test: `scripts/test_fof_badge_flash.py`
- Test: `scripts/test_verify_badge_usb_hardening.py`

### Step 1: Write failing result-attestation tests

Extend `UsbScannerFlowResult` tests to require:

- `stage_count` is exactly the number of frozen artifact stages and is in
  `[1, 3]`;
- `attempt_history` is an immutable tuple of defensive JSON copies;
- every attempt records ordinal, session, requested lanes, stage receipt,
  campaign terminal snapshot, outcome, classification, recovery action, and
  verified target;
- success history ends in `converged`;
- each earlier successful lane is linked to a receipt whose slot mask included
  it;
- fabricated, mutated, reordered, duplicate-session, digest-drifted, or
  incomplete histories fail production issuance/revalidation.

Keep `stage_receipt` as the latest receipt for compatibility. Add an immutable
`stage_receipts` property for the complete ordered set.

### Step 2: Implement production-issued history

Extend `_make_attested_usb_flow`'s private issued record with JSON-encoded
attempt and receipt tuples. Update its strict type/count checks and defensive
accessors. Normal one-attempt flows must retain their current observable
behavior with `stage_count == 1`.

Log one concise operator line per attempt and recovery without exposing a new
public mutation option.

### Step 3: Update hardening verifier tests first

Add failing verifier fixtures for:

- normal single-attempt flow;
- valid combined-then-one-lane retry;
- valid three-attempt same-lane recovery;
- retry that rewrites the successful peer;
- changed scanner identity;
- changed digest/size/target across receipts;
- missing prior convergence snapshot;
- more than three attempts;
- reset recovery without exact rebound proof.

### Step 4: Update the source/runtime verifier

Teach `scripts/verify_badge_usb_hardening.py` to:

- preserve the existing one-attempt assertions;
- verify every staged receipt against the same frozen scanner artifact;
- validate the latest live coordinator only against the latest slot mask;
- validate earlier lane convergence through immutable attempt snapshots;
- require final healthy status for all original scanner identities;
- report attempt count and recovery actions in its private evidence object.

Run:

```sh
python3 -m unittest scripts.test_verify_badge_usb_hardening -v
python3 scripts/verify_badge_usb_hardening.py
```

Expected: tests and static/source hardening verification pass.

### Step 5: Commit

```sh
git add \
  scripts/fof_badge_flash.py \
  scripts/test_fof_badge_flash.py \
  scripts/verify_badge_usb_hardening.py \
  scripts/test_verify_badge_usb_hardening.py
git commit -m "tools: attest bounded scanner retry history"
```

---

## Task 5: Full Automated Gate and Local Handoff

**Files:**

- No new production files expected.
- Update this plan's checkbox state only if the repository convention requires
  it; otherwise leave the committed plan unchanged.

### Step 1: Run focused host suites

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest scripts.test_fof_badge_flash -v
python3 -m unittest scripts.test_fof_badge_flash_phase_a_serial -v
python3 -m unittest scripts.test_verify_badge_usb_hardening -v
python3 scripts/verify_badge_usb_hardening.py
```

### Step 2: Run unchanged firmware artifact/version/native gates

These are read/build/test gates only; do not flash hardware:

```sh
python3 -m unittest discover -s scripts -p 'test_*.py' -v
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

If the complete script discovery includes environment-specific or unrelated
tests, record the exact failing test and run the repository's established
updater, artifact, and firmware-version subsets separately. Do not mask a
failure in a touched path.

### Step 3: Review the exact diff

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
git diff --check
git status --short
git diff --stat HEAD~4..HEAD
git diff HEAD~4..HEAD -- \
  scripts/fof_badge_flash.py \
  scripts/test_fof_badge_flash.py \
  scripts/verify_badge_usb_hardening.py \
  scripts/test_verify_badge_usb_hardening.py
```

Confirm:

- no firmware or Android files changed;
- no version macro changed;
- no public artifact changed;
- no unbounded retry loop exists;
- no identity or health gate was weakened;
- the camera file remains untouched.

### Step 4: Stop before physical acceptance

Report:

- local commit hashes;
- automated test counts and exact commands;
- final changed-file list;
- remaining attended test steps;
- confirmation that connected hardware was not mutated.

Do not bump to `0.67.0-badge-defcon34`, flash badges, publish a factory bundle,
or start game/Android validation until the operator explicitly authorizes the
final attended acceptance run.
