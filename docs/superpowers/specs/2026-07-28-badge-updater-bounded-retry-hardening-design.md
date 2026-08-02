# Badge Updater Bounded Retry Hardening Design

**Date:** 2026-07-28

**Status:** Approved design; implementation pending

**Scope:** Host-side USB flasher only

**Release target:** `0.67.0-badge-defcon34` after physical acceptance

## Objective

Make the existing single-uplink USB update path tolerate the transient scanner
and maintenance-lifecycle inconsistencies observed during the private
`0.64.92-badge-defcon34` three-badge canary. A recoverable lane failure must not
force an operator to diagnose, unplug, or manually restart the entire badge.

This hardening must preserve every existing safety invariant, avoid changes to
badge firmware and its memory footprint, and never rewrite a scanner lane that
has already converged.

## Non-goals

- No badge UI, detector, or theme changes.
- No Android app changes or Android-driven firmware mutation.
- No scanner or uplink firmware state-machine changes.
- No public push, tag, release, merge, or factory-bundle update.
- No release-version bump until the updater passes automated and attended
  hardware acceptance.

## Evidence and Failure Model

The private `.92` canary reached the target firmware on all three uplinks and
all six scanners with correct roles, healthy radios, clear rollback state, and
no scanner crashes. Three transient host-visible failures required manual
recovery:

1. A scanner exhausted readiness before its relay began. One targeted
   `fw_check_now` later allowed the exact lane to converge.
2. A failed campaign left an uplink in a committed `update_maintenance`
   session. A non-writing USB reset cleared the volatile lifecycle state.
3. A scanner exhausted its on-device relay attempts with
   `ota_ack_timeout`; its status then reported `fw_state=deferred` and
   `offer_manifest_mismatch`. Rebooting only that scanner through the existing
   recovery control cleared the volatile backoff, after which the same lane
   converged.

The host currently turns these recognized transient states into terminal
`FlashError` results. Its failure cleanup also always rethrows the original
error even when normal mode has been safely restored.

## Selected Approach

Implement a host-only retry coordinator around the existing maintenance
campaign. Keep the current combined first attempt for speed. If only one lane
fails with a recognized transient, restore the badge to normal operation,
recover that exact lane, and retry only that lane in a new maintenance session.

This approach adds no firmware RAM or flash usage, directly addresses the
failures already reproduced on hardware, and leaves the proven scanner and
uplink firmware unchanged.

Changing the scanner offer/backoff state machine is intentionally deferred. It
may be useful after DEFCON, but it would expand the firmware regression surface
while the release is in final validation.

## Retry Contract

### Budget

- Each requested scanner lane receives at most three host-level campaign
  attempts total, including its first attempt.
- Existing on-device relay retries remain unchanged.
- A lane that reaches `converged` or `current` is removed from the retry set and
  is never rewritten by a later attempt.
- A retry always uses the same frozen scanner artifact bytes, target version,
  SHA-256 digest, size, and original uplink binding.
- Every retry uses a new exact maintenance session identifier.

### Retryable conditions

The coordinator may retry only when the available evidence identifies one of
these transient conditions:

- readiness exhausted with zero relay attempts;
- `ota_ack_timeout`;
- `offer_manifest_mismatch`;
- scanner firmware state `deferred` while update backoff is active;
- temporary loss of the expected scanner during its reboot/reannouncement
  window;
- serial read or transport interruption while the same bound uplink is
  rebooting or transitioning between normal and maintenance modes.

Unknown errors are not automatically classified as transient.

### Terminal conditions

The updater must stop without another mutation if any of these occur:

- uplink USB serial number, hardware ID, or physical-location binding changes;
- scanner MAC address, physical lane, firmware role, or expected board role
  changes;
- staged target, SHA-256, size, generation, slot mask, or committed-byte count
  changes;
- a downgrade or unexpected `newer_skipped` result is observed;
- rollback, pending-verification, safe-mode, crash, partition, or health gates
  fail;
- the persisted game seed or configured role changes;
- a scanner identity cannot be conclusively reproved;
- the per-lane retry budget is exhausted.

## Recovery Sequence

For a retryable failed lane, the coordinator performs these ordered steps:

1. Snapshot the immutable uplink binding, both scanner identities, requested
   target, successfully converged lanes, and persisted game state.
2. Wait briefly for an in-flight worker or reboot to settle and reconcile fresh
   status before taking recovery action.
3. Finish or abort only the exact owned maintenance session, then prove the
   same uplink has returned to normal mode.
4. If the exact session cannot be cleared, permit one non-writing USB reset of
   the bound uplink. Rebind only when the same physical location and hardware
   identity return, and then reprove normal status and persisted game state.
5. For a scanner backoff, manifest mismatch, or exhausted relay attempt,
   issue the existing exact-lane `scanner_recovery` control that reboots that
   scanner into normal operation.
6. Wait for the same scanner MAC, expected lane, role, and healthy normal-mode
   status to reappear.
7. Start a new maintenance session and stage the same frozen artifact for only
   the still-outdated lane.
8. Run the existing strict convergence, post-reboot, rollback, radio-health,
   theme-control, identity, and game-state proofs.

The uplink USB-reset fallback may run at most once during one flasher
invocation. It performs no flash write and cannot authorize a different
descriptor or physical device.

## Control Flow and Result Model

The current failure recovery helper will be split conceptually into:

- a recovery operation that either returns a fully validated normal-mode
  snapshot or raises an invariant failure; and
- a retry coordinator that decides whether the original campaign error is
  classified, whether a retry budget remains, and which unresolved lane is
  eligible.

The coordinator owns the total attempt history. It will keep successful lanes
out of subsequent stage slot masks and will pass only unresolved lane
identities into the next campaign.

Final success remains strict: all originally requested older lanes must prove
convergence to the frozen target, and every requested lane must pass the
existing final scanner and uplink verification gates.

## Operator Output and History

The interactive flasher should show concise, actionable transitions:

- failed lane and classified transient reason;
- current attempt number out of three;
- maintenance cleanup and whether the USB-reset fallback was needed;
- exact scanner MAC being recovered;
- successful lanes being preserved;
- retry result and final verification.

The batch history JSON should record an ordered attempt list containing the
lane, maintenance session, failure classification, recovery action, result,
and final verified version. It must not claim success until the existing final
hardware proofs pass.

## Verification

### Automated tests

Add regression coverage for:

- zero-attempt readiness recovery and targeted retry;
- `ota_ack_timeout` followed by exact-lane scanner recovery;
- `offer_manifest_mismatch`/deferred backoff classification;
- successful peer lane excluded from the retry stage mask;
- one lost maintenance abort reconciled normally;
- one stale maintenance session recovered through the bounded non-writing USB
  reset;
- same uplink and scanner identities required after every recovery;
- retry budget exhaustion preserving the complete attempt history;
- all terminal invariant failures refusing recovery;
- game seed and configured role preserved across retry;
- no retry or reset on an already successful campaign.

All existing flasher, firmware artifact, native firmware, and version tests
remain required.

### Hardware acceptance

Use the connected three-badge setup to prove:

1. A normal full-badge update still converges without recovery.
2. A controlled transient failure is automatically recovered without cable
   movement or manual scanner flashing.
3. Only the failed lane is retried.
4. The other scanner continues to retain its verified target.
5. Uplink and scanner MAC/role continuity, rollback clearance, radio health,
   USB controls, and persisted configuration all survive.
6. Final status is healthy on all nine boards.

Only after those gates and the separate attended game/Android acceptance are
complete should the final firmware be named
`0.67.0-badge-defcon34` and considered for the public factory bundle.

## Rollback

The change is confined to the host flasher. If its retry coordinator fails
validation, revert the host-side commit and continue using the already-proven
manual targeted-lane recovery procedure with `.92`. No badge firmware rollback
is required because this design changes no firmware bytes.
